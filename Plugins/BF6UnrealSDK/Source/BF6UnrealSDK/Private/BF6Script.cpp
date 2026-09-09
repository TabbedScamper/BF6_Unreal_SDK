#include "BF6Script.h"
#include "BF6GameLog.h"   // finds the game log folder, whose name is mojibake on disk
#include "BF6ScriptBridge.h"
#include "BF6EditorOverlay.h"    // ---- BF6EditorOverlay ---- the full-screen host
#include "BF6PortalWeb.h"
#include "DesktopPlatformModule.h"
#include "IDesktopPlatform.h"
#include "Misc/Base64.h"
#include "Interfaces/IAudioFormat.h"   // FSoundQualityInfo, filled in by the decoder
#include "Decoders/VorbisAudioInfo.h"
#include "Sound/SoundWaveProcedural.h"
#include "Editor.h"
#include "BF6UiSound.h"
#include "BF6Project.h"        // the experience folder a save belongs to
#include "BF6Blocks.h"           // OPEN THIS IN THE BLOCK EDITOR: the same seam the UI builder uses
#include "BF6Assist.h"           // the user's own AI, asked from the editor's right-click menu
#include "BF6BuildMode.h"
#include "BF6Internal.h"
#include "BF6Theme.h"
#include "BF6SDKExtension.h"

#include "SWebBrowser.h"
#include "IWebBrowserWindow.h"
#include "IWebBrowserSingleton.h"
#include "WebBrowserModule.h"

#include "Containers/Ticker.h"
#include "Dom/JsonObject.h"
#include "Framework/Application/SlateApplication.h"
#include "Framework/Docking/TabManager.h"
#include "GameFramework/Actor.h"
#include "HAL/FileManager.h"
#include "HAL/IConsoleManager.h"
#include "HAL/PlatformFileManager.h"
#include "HAL/PlatformProcess.h"
#include "Interfaces/IPluginManager.h"
#include "Misc/ConfigCacheIni.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Misc/SecureHash.h"          // FMD5: the build proof hashes inputs and output
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "UObject/Package.h"          // GetTransientPackage returns a UPackage*, which NewObject needs whole
#include "UObject/StrongObjectPtr.h"
#include "Widgets/Docking/SDockTab.h"
#include "Widgets/Images/SImage.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Layout/SBorder.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/SNullWidget.h"
#include "Widgets/Text/STextBlock.h"
#include "WorkspaceMenuStructure.h"
#include "WorkspaceMenuStructureModule.h"

DEFINE_LOG_CATEGORY(LogBF6Script);

// ============================================================================
// Everything in this file is module-internal. The header is the whole of the
// surface, and the page is the whole of the UI.
// ============================================================================
namespace
{
	const FName   kTabId(TEXT("BF6Script"));
	const FString kBridgeName(TEXT("bf6script"));
	const FString kSiteScriptId(TEXT("bf6script.sync"));

	// ---- the tab and its browser -------------------------------------------
	TSharedPtr<IWebBrowserWindow> GWindow;
	TSharedPtr<SWebBrowser>       GBrowser;
	TSharedPtr<SBox>              GHost;
	TWeakPtr<SDockTab>            GTab;
	bool                          GPageReady = false;
	FString                       GUnavailableWhy;

	TStrongObjectPtr<UBF6ScriptBridge> GBridge;
	TArray<IConsoleObject*>            GCmds;

	// ---- what we know -------------------------------------------------------
	FString GNodeExe;         // full path to node.exe, or empty
	FString GNpmCli;          // full path to npm-cli.js, or empty
	FString GTemplateDir;     // the installed template, or empty
	FString GProjectDir;      // the open project, or empty
	FString GProjectName;
	FString GExperienceId;    // the experience the open project belongs to, or empty

	// WHICH PROJECT, AS A NUMBER THE PAGE CAN QUOTE BACK.
	//
	// A write from the page named a rel path and nothing else, and this side
	// resolved it against whatever GProjectDir happened to be. That is the same
	// thing only while the page and the tool agree about which project is open,
	// and they can stop agreeing without either noticing: an open this side
	// CARRIED OUT whose reply was lost leaves the page still showing the old
	// project, and its next save arrives here as a rel path that now names a
	// file in the new one. Project A's text, written into project B.
	//
	// So the page is told which project it is in and stamps every request that
	// means "in the project I have open" with the path and this counter. The
	// counter moves on every open, so reopening the SAME directory is still a
	// different generation and a request in flight across it is still stale.
	uint32  GProjectGen = 0;
	FString GPageMode;        // what the page says its language service is doing
	FString GSiteState;       // last sitestate line, for LogStatus

	// ---- work in flight -----------------------------------------------------
	struct FRun
	{
		FProcHandle Handle;
		void*   Read = nullptr;
		void*   Write = nullptr;
		FString Out;
		FString Label;
		// WHICH PROJECT THIS RUN BELONGS TO.
		//
		// A build is a separate process that outlives the click that started
		// it. Open another project while it runs and the completion handler
		// used to commit against whatever GProjectDir had become: project A's
		// build wrote project B's pending state and line map.
		FString ProjectDir;
		int32   ReqId = 0;

		// When this run exits zero, start this one instead of answering. BUILD is
		// a type check followed by the bundler, and the two are one request to
		// whoever pressed the button.
		FString NextLabel;
		FString NextArgs;

		// THE INPUT MANIFEST AS IT WAS BEFORE THE COMPILER READ ANYTHING.
		//
		// The proof of a checked build used to be taken only at the END, so a
		// source file edited while tsc or the bundler was reading it produced a
		// hash of files that were never compiled: a new input certified against
		// an old bundle. This is taken before the type check starts and only
		// CONFIRMED at the end, so a build whose inputs moved under it fails
		// instead of certifying them. It rides across both stages of a build,
		// because the type check and the bundle are one verdict.
		FString InputsAtStart;

		// When the process started, so "the bundler wrote dist/bundle.ts" can be
		// told apart from "a bundle.ts from some earlier run is lying there".
		//
		// IN UTC, because the only thing it is ever compared against is
		// IFileManager::GetTimeStamp, which is UTC. FDateTime::Now() is local
		// time, and east of Greenwich that comparison would call every freshly
		// written bundle stale by the size of the time zone offset.
		FDateTime Started;

		FTSTicker::FDelegateHandle Tick;
	};
	TSharedPtr<FRun> GRun;

	// ---- the proof that the open project has a CHECKED build ----------------
	//
	// A bundler exiting zero is not evidence that the code type checks. See THE
	// CHECKED BUILD below. These three are that evidence, and PUSH will not send
	// a bundle without them.
	FString GCheckedInputs;   // hash of every compiled input, as the check saw them
	FString GCheckedBundle;   // hash of dist/bundle.ts, as that build produced it
	// PUSH sends dist/bundle.strings.json beside the bundle, and nothing used to
	// check it. It could be edited or deleted after a checked build and the
	// project still reported itself ready to send, so half of what left here was
	// certified and half was whatever happened to be on disk. Either a hash, or
	// the word "none" for a build that produced no strings file at all: the
	// difference matters, because one appearing later is also a change.
	FString GCheckedStrings;
	// WHICH COMPILER SAID SO. package-lock.json is in the input manifest, so the
	// project's own dependency versions are covered. This is the part that is
	// not in the project: the node executable and the TypeScript compiler the
	// check actually ran under.
	FString GCheckedToolchain;
	FString GCheckedAt;

	// Large payloads leave in order, a few chunks per tick, so a two megabyte
	// push never stalls the editor's frame.
	struct FChunkJob
	{
		FString Kind;
		FString Path;      // for a type file: which one
		FString Data;
		int32   Sent = 0;
	};
	TArray<FChunkJob>          GChunks;
	FTSTicker::FDelegateHandle GChunkTick;
	const int32                kChunkChars = 96 * 1024;

	// ---- the site half ------------------------------------------------------
	int32   GSiteReq = 0;          // editor request id waiting on the site
	FString GSiteOp;               // which one
	bool    GSiteOnScriptPage = false;
	// WHICH experience the browser is actually on, as opposed to whether it is
	// on a Script page at all. Without this the only destination check was
	// "some Script page is open", so a project linked to one experience could
	// be pushed onto another that merely happened to be on screen.
	FString GSitePageExperience;

	// WHICH PAGE, not just which experience.
	//
	// The experience id alone cannot tell a reply from the page the request was
	// sent to apart from a reply from a page that was loaded afterwards. The
	// counter moves whenever the page identity moves: a different experience, a
	// different url, or the panel leaving the Script page at all. A request
	// records the value it was sent under, and an answer arriving under a
	// different one is a stale answer about a page that no longer exists.
	uint32  GSitePageGen = 0;
	FString GSitePageUrl;

	// What the request in flight was bound to when it left. Both are checked
	// before its answer is allowed to change anything.
	uint32  GSiteReqGen = 0;
	FString GSiteReqExperience;

	// ---- the log tail -------------------------------------------------------
	bool                       GTailing = false;
	int64                      GTailOffset = 0;
	FString                    GTailPath;
	FTSTicker::FDelegateHandle GTailTick;

	// ---- the scene selection ------------------------------------------------
	int32                      GLastSelObj = -1;
	FTSTicker::FDelegateHandle GSelTick;
}

// ---------------------------------------------------------------------------
// JSON and JS helpers.
//
// Everything the tool says to a page goes through one of these. Nothing is
// ever concatenated into JavaScript unescaped: a project name with a quote in
// it would otherwise be a broken page at best.
// ---------------------------------------------------------------------------
static FString JsQuote(const FString& In)
{
	FString Out;
	Out.Reserve(In.Len() + 16);
	Out.AppendChar(TEXT('"'));
	for (int32 i = 0; i < In.Len(); ++i)
	{
		const TCHAR C = In[i];
		switch (C)
		{
		case TEXT('"'):  Out += TEXT("\\\""); break;
		case TEXT('\\'): Out += TEXT("\\\\"); break;
		case TEXT('\n'): Out += TEXT("\\n"); break;
		case TEXT('\r'): Out += TEXT("\\r"); break;
		case TEXT('\t'): Out += TEXT("\\t"); break;
		default:
			// Control characters, and the two line separators JavaScript
			// treats as newlines inside a string literal even though JSON
			// does not.
			if (C < 0x20 || C == 0x2028 || C == 0x2029)
			{
				Out += FString::Printf(TEXT("\\u%04x"), (int32)C);
			}
			else
			{
				Out.AppendChar(C);
			}
			break;
		}
	}
	Out.AppendChar(TEXT('"'));
	return Out;
}

static FString JsonOf(const TSharedRef<FJsonObject>& Obj)
{
	FString Out;
	TSharedRef<TJsonWriter<>> W = TJsonWriterFactory<>::Create(&Out);
	FJsonSerializer::Serialize(Obj, W);
	return Out;
}

static TSharedPtr<FJsonObject> ParseJson(const FString& Text)
{
	TSharedPtr<FJsonObject> Obj;
	TSharedRef<TJsonReader<>> R = TJsonReaderFactory<>::Create(Text);
	if (!FJsonSerializer::Deserialize(R, Obj)) return nullptr;
	return Obj;
}

// Declared here because the build's completion reaches for all three, and it
// is defined above the project plumbing they belong with.
static int32 BuildLineMap();
static void  WritePending(const FString& State, const FString& Detail);
static void  SendPending();
// RunTick starts the second stage of a two-stage build, and StartRun is below it.
static bool  StartRun(const FString& Exe, const FString& Args, const FString& Cwd,
	const FString& Label, int32 ReqId, FString& OutWhy,
	const FString& NextLabel, const FString& NextArgs);
// THE CHECKED BUILD's helpers. RunTick decides a build's verdict and sits above
// them, so they are named here rather than moved and losing the section they
// are explained in.
static FString HashProjectInputs(const FString& Dir, FString* OutWhy = nullptr);
static FString HashOneFile(const FString& Full);
static FString BundlePathFor(const FString& Dir);
static FString StringsPathFor(const FString& Dir);
static bool    HashStringsArtifact(const FString& Dir, FString& OutHash, FString& OutWhy);
static FString ToolchainId(const FString& Dir);
static bool    BundlerDisownedTheOutput(const FString& Out, FString& OutWhy);

// Run JavaScript on the tool's OWN page. Nothing here reaches the site.
static void ExecEditor(const FString& Js)
{
	if (!GWindow.IsValid() || !GWindow->IsValid()) return;
	GWindow->ExecuteJavascript(Js);
}

static void ReplyRaw(int32 Id, const FString& Fields)
{
	ExecEditor(FString::Printf(TEXT("try{window.BF6ScriptEditor.reply({id:%d,%s})}catch(e){}"), Id, *Fields));
}

static void Reply(int32 Id, const TSharedRef<FJsonObject>& Obj)
{
	Obj->SetNumberField(TEXT("id"), Id);
	ExecEditor(FString::Printf(TEXT("try{window.BF6ScriptEditor.reply(%s)}catch(e){}"), *JsonOf(Obj)));
}

static void ReplyOk(int32 Id, const FString& Text = FString())
{
	TSharedRef<FJsonObject> O = MakeShared<FJsonObject>();
	O->SetBoolField(TEXT("ok"), true);
	if (!Text.IsEmpty()) O->SetStringField(TEXT("text"), Text);
	Reply(Id, O);
}

static void ReplyFail(int32 Id, const FString& Why)
{
	TSharedRef<FJsonObject> O = MakeShared<FJsonObject>();
	O->SetBoolField(TEXT("ok"), false);
	O->SetStringField(TEXT("why"), Why);
	Reply(Id, O);
	UE_LOG(LogBF6Script, Warning, TEXT("%s"), *Why);
}

static void Event(const TSharedRef<FJsonObject>& Obj)
{
	ExecEditor(FString::Printf(TEXT("try{window.BF6ScriptEditor.event(%s)}catch(e){}"), *JsonOf(Obj)));
}

static void Progress(const FString& Text, const TCHAR* Level = TEXT("d"))
{
	TSharedRef<FJsonObject> O = MakeShared<FJsonObject>();
	O->SetStringField(TEXT("kind"), TEXT("progress"));
	O->SetStringField(TEXT("text"), Text);
	O->SetStringField(TEXT("level"), Level);
	Event(O);
}

// ---------------------------------------------------------------------------
// Chunked payloads.
// ---------------------------------------------------------------------------
static bool ChunkTick(float)
{
	int32 Budget = 6;   // chunks per tick: about half a megabyte, comfortably
	while (Budget-- > 0 && GChunks.Num() > 0)
	{
		FChunkJob& J = GChunks[0];
		const int32 Total = FMath::Max(1, FMath::DivideAndRoundUp(J.Data.Len(), kChunkChars));
		const int32 Start = J.Sent * kChunkChars;
		const int32 Len = FMath::Min(kChunkChars, J.Data.Len() - Start);
		const FString Piece = J.Data.Mid(Start, FMath::Max(0, Len));

		ExecEditor(FString::Printf(
			TEXT("try{window.BF6ScriptEditor.push({kind:%s,path:%s,seq:%d,of:%d,data:%s})}catch(e){}"),
			*JsQuote(J.Kind), *JsQuote(J.Path), J.Sent, Total, *JsQuote(Piece)));

		if (++J.Sent >= Total) GChunks.RemoveAt(0);
	}

	if (GChunks.Num() == 0)
	{
		GChunkTick.Reset();
		return false;
	}
	return true;
}

static void PushPayload(const FString& Kind, const FString& Path, const FString& Data)
{
	FChunkJob J;
	J.Kind = Kind;
	J.Path = Path;
	J.Data = Data;
	GChunks.Add(MoveTemp(J));
	if (!GChunkTick.IsValid())
	{
		GChunkTick = FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateStatic(&ChunkTick), 0.f);
	}
}

// ---------------------------------------------------------------------------
// Paths.
// ---------------------------------------------------------------------------
static FString ScriptRes()
{
	return FPaths::Combine(g_pluginDir, TEXT("Resources"), TEXT("script"));
}

// The curated answers, which are a different thing from the mined faq.json
// beside them: fifteen questions per theme, each with a checked TypeScript
// example and a loadable block workspace. They live outside Resources/script
// because the blocks editor reads the same three files.
static FString AnswersRes()
{
	return FPaths::Combine(g_pluginDir, TEXT("Resources"), TEXT("faq"), TEXT("answers"));
}

// Where a curated answer's block example is dropped so the blocks editor can
// load it by name. The same folder shape the UI builder already writes into.
static FString BlocksSnippetDir()
{
	return FPaths::Combine(g_pluginDir, TEXT("Resources"), TEXT("blocks"), TEXT("snippets"), TEXT("faq"));
}

static FString ScriptSaved()
{
	return FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("BF6UnrealSDK"), TEXT("script"));
}

static FString GeneratedDir()
{
	return FPaths::Combine(ScriptSaved(), TEXT("_generated"));
}

FString BF6Script::ProjectDirForSave(const FString& Level, const FString& Save)
{
	// An experience is one game mode: one script project shared by every map in
	// its rotation. When the save belongs to one, its script lives in the
	// experience folder, beside the settings and the workspace it goes with,
	// and not in a third place of its own. Empty means this save belongs to no
	// experience, and everything below is unchanged.
	const FString Owned = BF6Project::ScriptDirForSave(Level, Save);
	if (!Owned.IsEmpty()) return Owned;

	const FString Url = BF6PortalWeb::ExperienceForSave(Level, Save);
	if (!Url.IsEmpty())
	{
		// The experience id is the uuid in the link the panel recorded.
		FString Id;
		int32 At = Url.Find(TEXT("id="));
		if (At != INDEX_NONE)
		{
			Id = Url.Mid(At + 3);
			int32 Amp; if (Id.FindChar(TEXT('&'), Amp)) Id.LeftInline(Amp);
		}
		if (Id.Len() >= 32)
		{
			return FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("BF6UnrealSDK"),
				TEXT("portal"), TEXT("experiences"), Id, TEXT("script"));
		}
	}
	const FString Name = Save.IsEmpty() ? TEXT("untitled") : Save;
	return FPaths::Combine(ScriptSaved(), Name);
}

// ---------------------------------------------------------------------------
// Finding node, npm and the template.
//
// None of the three is fatal. The editor, the recipes, the explanations, the
// community answers and CHECK MY SCRIPT all work with none of them installed;
// only NEW, INSTALL and BUILD need node, and the page is told exactly which
// piece is missing rather than a button quietly doing nothing.
// ---------------------------------------------------------------------------
static void FindNode()
{
	GNodeExe.Reset();
	GNpmCli.Reset();

	FString Configured;
	GConfig->GetString(TEXT("BF6UnrealSDK"), TEXT("NodeExe"), Configured, GEngineIni);

	TArray<FString> Tries;
	if (!Configured.IsEmpty()) Tries.Add(Configured);
	Tries.Add(TEXT("C:/Program Files/nodejs/node.exe"));
	Tries.Add(TEXT("C:/Program Files (x86)/nodejs/node.exe"));
	const FString AppData = FPlatformMisc::GetEnvironmentVariable(TEXT("LOCALAPPDATA"));
	if (!AppData.IsEmpty())
	{
		Tries.Add(FPaths::Combine(AppData, TEXT("Programs"), TEXT("nodejs"), TEXT("node.exe")));
		Tries.Add(FPaths::Combine(AppData, TEXT("nvm"), TEXT("node.exe")));
	}

	for (const FString& T : Tries)
	{
		if (FPaths::FileExists(T)) { GNodeExe = T; break; }
	}

	if (GNodeExe.IsEmpty())
	{
		UE_LOG(LogBF6Script, Warning,
			TEXT("Node was not found. BUILD, INSTALL and NEW need it; everything else in the panel works without it. ")
			TEXT("Install it from nodejs.org, or set [BF6UnrealSDK] NodeExe in the engine config to the full path of node.exe."));
		return;
	}

	const FString Dir = FPaths::GetPath(GNodeExe);
	const FString Cli = FPaths::Combine(Dir, TEXT("node_modules"), TEXT("npm"), TEXT("bin"), TEXT("npm-cli.js"));
	if (FPaths::FileExists(Cli)) GNpmCli = Cli;

	UE_LOG(LogBF6Script, Display, TEXT("Node: %s. npm: %s"), *GNodeExe,
		GNpmCli.IsEmpty() ? TEXT("not found beside node, npm run will not work") : *GNpmCli);
}

static void FindTemplate()
{
	GTemplateDir.Reset();

	FString Configured;
	GConfig->GetString(TEXT("BF6UnrealSDK"), TEXT("ScriptTemplateDir"), Configured, GEngineIni);
	if (!Configured.IsEmpty() && FPaths::DirectoryExists(Configured))
	{
		GTemplateDir = Configured;
		return;
	}

	// The template ships inside the Portal SDK download, under
	// GodotProject/User_Created/projects/_template-<version>. Look for the
	// newest one under any PortalSDK root on the machine's fixed drives.
	TArray<FString> Roots;
	for (TCHAR Drive = TEXT('C'); Drive <= TEXT('F'); ++Drive)
	{
		const FString Base = FString::Printf(TEXT("%c:/"), Drive);
		if (!FPaths::DirectoryExists(Base)) continue;
		TArray<FString> Found;
		IFileManager::Get().FindFiles(Found, *(Base + TEXT("PortalSDK*")), false, true);
		for (const FString& F : Found) Roots.Add(Base + F);
	}

	FString Best;
	for (const FString& Root : Roots)
	{
		const FString Projects = FPaths::Combine(Root, TEXT("GodotProject"), TEXT("User_Created"), TEXT("projects"));
		if (!FPaths::DirectoryExists(Projects)) continue;
		TArray<FString> Dirs;
		IFileManager::Get().FindFiles(Dirs, *(Projects / TEXT("_template*")), false, true);
		for (const FString& D : Dirs)
		{
			const FString Full = FPaths::Combine(Projects, D);
			if (FPaths::FileExists(FPaths::Combine(Full, TEXT("package.json"))) && Full > Best) Best = Full;
		}
	}

	GTemplateDir = Best;
	if (GTemplateDir.IsEmpty())
	{
		UE_LOG(LogBF6Script, Warning,
			TEXT("The Portal scripting template was not found. NEW cannot make a project until it is. ")
			TEXT("Install the Portal SDK, or set [BF6UnrealSDK] ScriptTemplateDir to the folder holding the template's package.json."));
	}
	else
	{
		UE_LOG(LogBF6Script, Display, TEXT("Script template: %s"), *GTemplateDir);
	}
}

// ---------------------------------------------------------------------------
// Running node, with its output going to the page as it arrives.
// ---------------------------------------------------------------------------
static bool RunTick(float)
{
	if (!GRun.IsValid()) return false;
	FRun& R = *GRun;

	const FString Fresh = FPlatformProcess::ReadPipe(R.Read);
	if (!Fresh.IsEmpty())
	{
		R.Out += Fresh;
		TArray<FString> Lines;
		Fresh.ParseIntoArrayLines(Lines, false);
		for (const FString& L : Lines)
		{
			if (L.TrimStartAndEnd().IsEmpty()) continue;
			UE_LOG(LogBF6Script, Display, TEXT("[%s] %s"), *R.Label, *L);
			const bool bBad = L.Contains(TEXT("error")) || L.Contains(TEXT("ERR!")) || L.Contains(TEXT("Error:"));
			Progress(L, bBad ? TEXT("e") : TEXT("d"));
		}
	}

	if (FPlatformProcess::IsProcRunning(R.Handle)) return true;

	// Drain whatever landed between the last read and the exit.
	R.Out += FPlatformProcess::ReadPipe(R.Read);

	int32 Code = 0;
	FPlatformProcess::GetProcReturnCode(R.Handle, &Code);
	FPlatformProcess::CloseProc(R.Handle);
	FPlatformProcess::ClosePipe(R.Read, R.Write);

	const int32 ReqId = R.ReqId;
	const FString Label = R.Label;
	const FString Out = R.Out;
	const FString RanFor = R.ProjectDir;
	const FString NextLabel = R.NextLabel;
	const FString NextArgs = R.NextArgs;
	const FString InputsAtStart = R.InputsAtStart;
	const FDateTime Started = R.Started;
	GRun.Reset();

	// The project moved on while this ran. Its result belongs to the project
	// it was started in and to nothing else, so it is reported and dropped
	// rather than written into whatever is open now.
	const bool bSameProject = RanFor.IsEmpty() || RanFor == GProjectDir;
	if (!bSameProject)
	{
		UE_LOG(LogBF6Script, Warning,
			TEXT("%s finished for %s, but %s is open now. The result was not applied."),
			*Label, *RanFor, GProjectDir.IsEmpty() ? TEXT("no project") : *GProjectDir);
		Progress(FString::Printf(
			TEXT("%s finished for the project you left. Open it again and rebuild to use the result."),
			*Label), TEXT("w"));
		if (ReqId) ReplyFail(ReqId, TEXT("that build belonged to the project you switched away from"));
		return false;
	}

	if (Code != 0)
	{
		UE_LOG(LogBF6Script, Error, TEXT("%s failed with exit code %d."), *Label, Code);
		// A stage that failed leaves nothing send-ready behind it. Without this
		// a failed rebuild would leave the PREVIOUS build's proof standing and
		// PUSH would still offer to send the older bundle as if it were current.
		if (Label == TEXT("typecheck") || Label == TEXT("build"))
		{
			GCheckedInputs.Reset();
			GCheckedBundle.Reset();
			GCheckedStrings.Reset();
			GCheckedToolchain.Reset();
			GCheckedAt.Reset();
			WritePending(TEXT("failed"), Label == TEXT("typecheck")
				? FString(TEXT("The type check found errors, so nothing was bundled. Fix them and press BUILD again."))
				: FString(TEXT("The bundler failed. Nothing is ready to send.")));
			SendPending();
		}
		if (ReqId)
		{
			TSharedRef<FJsonObject> O = MakeShared<FJsonObject>();
			O->SetBoolField(TEXT("ok"), false);
			O->SetStringField(TEXT("why"), Label == TEXT("typecheck")
				? FString(TEXT("The type check found errors in the source, so nothing was bundled."))
				: FString::Printf(TEXT("%s failed with exit code %d."), *Label, Code));
			O->SetStringField(TEXT("text"), Out.Right(4000));
			Reply(ReqId, O);
		}
		return false;
	}

	UE_LOG(LogBF6Script, Display, TEXT("%s finished."), *Label);

	if (Label == TEXT("build"))
	{
		// EXIT ZERO IS WHERE THE CHECKING STARTS, NOT WHERE IT ENDS. See THE
		// CHECKED BUILD above for the reproduction this guards against.
		FString Why;
		bool bBad = BundlerDisownedTheOutput(Out, Why);
		const FString BundlePath = BundlePathFor(RanFor);
		if (!bBad && !FPaths::FileExists(BundlePath))
		{
			bBad = true;
			Why = TEXT("it did not write dist/bundle.ts at all");
		}
		// Not merely present: written by THIS run. A bundle left over from an
		// earlier build would otherwise be pushed as though it were the one just
		// asked for. One second of slack, because file times are coarse.
		if (!bBad && IFileManager::Get().GetTimeStamp(*BundlePath) + FTimespan::FromSeconds(1.0) < Started)
		{
			bBad = true;
			Why = TEXT("dist/bundle.ts is older than the build that claimed to write it");
		}

		// WHAT WAS CHECKED HAS TO STILL BE WHAT IS ON DISK.
		//
		// InputsAtStart was taken before the type check was started. Comparing
		// it here is what stops a source file edited mid build from being
		// certified by a compile that never read it. The one case this cannot
		// see is an edit that is undone again before the build ends, which
		// hashes the same; that is accepted, because the alternative is holding
		// the user's own files open for the length of a build.
		FString InputsWhy;
		const FString InputsNow = HashProjectInputs(RanFor, &InputsWhy);
		if (!bBad && InputsAtStart.IsEmpty())
		{
			bBad = true;
			Why = TEXT("the tool did not record what it was building from, so it cannot certify the result");
		}
		if (!bBad && InputsNow.IsEmpty())
		{
			bBad = true;
			Why = FString::Printf(TEXT("the project's files could not all be read afterwards: %s"), *InputsWhy);
		}
		if (!bBad && InputsNow != InputsAtStart)
		{
			bBad = true;
			Why = TEXT("the project's files changed while it was being built, so what was checked is not what is on disk now");
		}

		// The strings artifact is sent with the bundle, so it is part of what
		// this build is certifying, whether it produced one or not.
		FString StringsHash, StringsWhy;
		if (!bBad && !HashStringsArtifact(RanFor, StringsHash, StringsWhy))
		{
			bBad = true;
			Why = StringsWhy;
		}

		const FString BundleHash = bBad ? FString() : HashOneFile(BundlePath);
		if (!bBad && BundleHash.IsEmpty())
		{
			bBad = true;
			Why = TEXT("dist/bundle.ts could not be read back after it was written");
		}

		if (bBad)
		{
			GCheckedInputs.Reset();
			GCheckedBundle.Reset();
			GCheckedStrings.Reset();
			GCheckedToolchain.Reset();
			GCheckedAt.Reset();
			const FString Said = FString::Printf(
				TEXT("The bundler exited without an error code, but %s. Nothing is ready to send."), *Why);
			UE_LOG(LogBF6Script, Error, TEXT("%s"), *Said);
			Progress(Said, TEXT("e"));
			WritePending(TEXT("failed"), Said);
			SendPending();
			if (ReqId)
			{
				TSharedRef<FJsonObject> O = MakeShared<FJsonObject>();
				O->SetBoolField(TEXT("ok"), false);
				O->SetStringField(TEXT("why"), Said);
				O->SetStringField(TEXT("text"), Out.Right(4000));
				Reply(ReqId, O);
			}
			return false;
		}

		// PUBLISH MODE, ON THE THING THAT IS ACTUALLY PUBLISHED.
		//
		// After the bundler, because the bundle is what gets sent, and before
		// the build is called finished, because a publish build that cannot
		// produce its lean bundle has not succeeded at what was asked.
		if (BF6Script::PublishMode(RanFor))
		{
			int32 Removed = 0;
			FString LeanWhy;
			if (!BF6Script::MakeLeanBundle(RanFor, LeanWhy, Removed))
			{
				const FString Said = FString::Printf(
					TEXT("The build worked, but the publish copy was not made: %s. ")
					TEXT("Nothing is ready to send while publish mode is on."), *LeanWhy);
				UE_LOG(LogBF6Script, Error, TEXT("%s"), *Said);
				Progress(Said, TEXT("e"));
				GCheckedInputs.Reset();
				GCheckedBundle.Reset();
				GCheckedStrings.Reset();
				GCheckedToolchain.Reset();
				GCheckedAt.Reset();
				WritePending(TEXT("failed"), Said);
				SendPending();
				if (ReqId)
				{
					TSharedRef<FJsonObject> O = MakeShared<FJsonObject>();
					O->SetBoolField(TEXT("ok"), false);
					O->SetStringField(TEXT("why"), Said);
					Reply(ReqId, O);
				}
				return false;
			}
		}

		// The map from bundle lines back to the user's own files is built
		// HERE, against the bundle that was just produced, so a verdict
		// from the site can never be mapped through a stale one.
		const int32 Mapped = BuildLineMap();
		// InputsAtStart rather than InputsNow, even though the two were just
		// proved equal: this records what the compiler was given, which is the
		// thing the verdict is about.
		GCheckedInputs = InputsAtStart;
		GCheckedBundle = BundleHash;
		GCheckedStrings = StringsHash;
		GCheckedToolchain = ToolchainId(RanFor);
		GCheckedAt = FDateTime::Now().ToString(TEXT("%Y-%m-%d %H:%M:%S"));
		WritePending(TEXT("built"), FString::Printf(
			TEXT("Source type checked and bundle built. %d source file%s can be pointed at from a Portal error."),
			Mapped, Mapped == 1 ? TEXT("") : TEXT("s")));
		SendPending();
	}

	// The next stage of a two-stage build. The request stays open across it,
	// because from the outside BUILD is one button and one answer.
	if (!NextLabel.IsEmpty())
	{
		FString Why;
		if (!StartRun(GNodeExe, NextArgs, RanFor, NextLabel, ReqId, Why, FString(), FString()))
		{
			UE_LOG(LogBF6Script, Error, TEXT("%s"), *Why);
			if (ReqId) ReplyFail(ReqId, Why);
			return false;
		}
		// The manifest belongs to the whole build, not to the stage that took
		// it. Without this the bundle stage would reach its completion with
		// nothing to compare against and refuse to certify a good build.
		if (GRun.IsValid()) GRun->InputsAtStart = InputsAtStart;
		return false;
	}

	if (ReqId) ReplyOk(ReqId, Label + TEXT(" finished."));
	return false;
}

static bool StartRun(const FString& Exe, const FString& Args, const FString& Cwd,
	const FString& Label, int32 ReqId, FString& OutWhy,
	const FString& NextLabel, const FString& NextArgs)
{
	if (GRun.IsValid())
	{
		OutWhy = TEXT("Something is already running. Wait for it to finish.");
		return false;
	}
	if (!FPaths::FileExists(Exe))
	{
		OutWhy = FString::Printf(TEXT("Cannot run %s: it is not there."), *Exe);
		return false;
	}

	TSharedPtr<FRun> R = MakeShared<FRun>();
	R->Label = Label;
	R->ProjectDir = Cwd;
	R->ReqId = ReqId;
	R->NextLabel = NextLabel;
	R->NextArgs = NextArgs;
	R->Started = FDateTime::UtcNow();   // see FRun::Started: compared against a UTC file time
	if (!FPlatformProcess::CreatePipe(R->Read, R->Write))
	{
		OutWhy = TEXT("Could not open a pipe to read the output.");
		return false;
	}

	R->Handle = FPlatformProcess::CreateProc(*Exe, *Args, false, true, true, nullptr, 0, *Cwd, R->Write, nullptr);
	if (!R->Handle.IsValid())
	{
		FPlatformProcess::ClosePipe(R->Read, R->Write);
		OutWhy = FString::Printf(TEXT("Could not start %s."), *Exe);
		return false;
	}

	UE_LOG(LogBF6Script, Display, TEXT("%s: %s %s (in %s)"), *Label, *Exe, *Args, *Cwd);
	Progress(Label + TEXT(" started."), TEXT("d"));

	GRun = R;
	R->Tick = FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateStatic(&RunTick), 0.1f);
	return true;
}

// ---------------------------------------------------------------------------
// Making a project from the template.
// ---------------------------------------------------------------------------
static bool ShouldSkipTemplateFile(const FString& Rel)
{
	// node_modules is reinstalled, .git belongs to whoever cloned the template,
	// and dist is build output. Nothing else is filtered: the point is that the
	// project on disk IS the template.
	return Rel.StartsWith(TEXT("node_modules/")) || Rel.Contains(TEXT("/node_modules/"))
		|| Rel.StartsWith(TEXT(".git/")) || Rel.Contains(TEXT("/.git/"))
		|| Rel.StartsWith(TEXT("dist/")) || Rel.Contains(TEXT("/dist/"));
}

static FString KebabCase(const FString& In)
{
	FString Out;
	for (int32 i = 0; i < In.Len(); ++i)
	{
		const TCHAR C = FChar::ToLower(In[i]);
		if (FChar::IsAlnum(C)) Out.AppendChar(C);
		else if (C == TEXT(' ') || C == TEXT('_') || C == TEXT('-'))
		{
			if (Out.Len() && Out[Out.Len() - 1] != TEXT('-')) Out.AppendChar(TEXT('-'));
		}
	}
	while (Out.EndsWith(TEXT("-"))) Out.LeftChopInline(1);
	return Out.IsEmpty() ? TEXT("portal-experience") : Out;
}

// What the template's own scripts/init.js does, with the answers supplied
// rather than asked for. Kept deliberately close to it so a project made here
// and a project made by npm run init are the same project.
// ---- WHICH EXPERIENCE A PROJECT IS FOR, ON DISK ---------------------------
//
// MOD_ID in the project's .env is the template's OWN answer to that question,
// so the tool reads and writes that rather than inventing a second place for
// the same fact and letting the two disagree. The SESSION_ID beside it is
// never read or written: that one IS a credential.
static FString ReadEnvModId(const FString& Dir)
{
	FString Text;
	if (Dir.IsEmpty() || !FFileHelper::LoadFileToString(Text, *FPaths::Combine(Dir, TEXT(".env")))) return FString();
	TArray<FString> Lines;
	Text.ParseIntoArrayLines(Lines, false);
	for (FString L : Lines)
	{
		L.TrimStartAndEndInline();
		if (!L.StartsWith(TEXT("MOD_ID="))) continue;
		FString V = L.RightChop(7).TrimStartAndEnd();
		V.ReplaceInline(TEXT("\""), TEXT(""));
		V.ReplaceInline(TEXT("'"), TEXT(""));
		return V.TrimStartAndEnd();
	}
	return FString();
}

static void WriteEnvModId(const FString& Dir, const FString& ExperienceId)
{
	if (Dir.IsEmpty() || ExperienceId.IsEmpty()) return;
	const FString Env = FPaths::Combine(Dir, TEXT(".env"));
	FString Text;
	FFileHelper::LoadFileToString(Text, *Env);   // an absent .env starts empty
	if (Text.Contains(TEXT("MOD_ID=")))
	{
		TArray<FString> Lines;
		Text.ParseIntoArrayLines(Lines, false);
		for (FString& L : Lines)
		{
			if (L.TrimStart().StartsWith(TEXT("MOD_ID="))) L = FString::Printf(TEXT("MOD_ID=\"%s\""), *ExperienceId);
		}
		Text = FString::Join(Lines, TEXT("\n"));
	}
	else
	{
		Text += FString::Printf(TEXT("\nMOD_ID=\"%s\"\n"), *ExperienceId);
	}
	FFileHelper::SaveStringToFile(Text, *Env);
}

static bool ApplyInit(const FString& Dir, const FString& ExperienceName,
	const FString& Description, const FString& Boilerplate,
	const FString& ExperienceId, FString& OutWhy)
{
	IFileManager& FM = IFileManager::Get();

	// 1. .env from .env.example, if there is not one already.
	const FString Env = FPaths::Combine(Dir, TEXT(".env"));
	const FString EnvExample = FPaths::Combine(Dir, TEXT(".env.example"));
	if (!FPaths::FileExists(Env) && FPaths::FileExists(EnvExample))
	{
		FM.Copy(*Env, *EnvExample);
	}
	// The mod id is not a secret and the deploy path is not ours, but writing
	// it means a user who later runs npm run deploy from a terminal does not
	// have to look it up.
	if (FPaths::FileExists(Env)) WriteEnvModId(Dir, ExperienceId);

	// 2. Which of the two boilerplates.
	const FString IndexTs = FPaths::Combine(Dir, TEXT("src"), TEXT("index.ts"));
	const FString BoilerTs = FPaths::Combine(Dir, TEXT("src"), TEXT("boilerplate.ts"));
	if (Boilerplate == TEXT("example"))
	{
		if (FPaths::FileExists(BoilerTs)) FM.Delete(*BoilerTs);
	}
	else
	{
		if (FPaths::FileExists(BoilerTs))
		{
			if (FPaths::FileExists(IndexTs)) FM.Delete(*IndexTs);
			FM.Move(*IndexTs, *BoilerTs);
		}
	}

	// 3. package.json.
	const FString PkgPath = FPaths::Combine(Dir, TEXT("package.json"));
	FString PkgText;
	if (!FFileHelper::LoadFileToString(PkgText, *PkgPath))
	{
		OutWhy = TEXT("The copied template has no package.json.");
		return false;
	}
	TSharedPtr<FJsonObject> Pkg = ParseJson(PkgText);
	if (!Pkg.IsValid())
	{
		OutWhy = TEXT("The template's package.json could not be read as JSON.");
		return false;
	}

	FString TemplateVersion;
	Pkg->TryGetStringField(TEXT("version"), TemplateVersion);
	Pkg->SetStringField(TEXT("templateVersion"), TemplateVersion);
	Pkg->SetStringField(TEXT("name"), KebabCase(ExperienceName));
	Pkg->SetStringField(TEXT("experienceName"), ExperienceName);
	Pkg->SetStringField(TEXT("version"), TEXT("1.0.0"));
	Pkg->SetStringField(TEXT("description"), Description);
	Pkg->RemoveField(TEXT("repository"));
	Pkg->RemoveField(TEXT("bugs"));
	Pkg->RemoveField(TEXT("homepage"));

	FString OutPkg;
	TSharedRef<TJsonWriter<TCHAR, TPrettyJsonPrintPolicy<TCHAR>>> W =
		TJsonWriterFactory<TCHAR, TPrettyJsonPrintPolicy<TCHAR>>::Create(&OutPkg);
	FJsonSerializer::Serialize(Pkg.ToSharedRef(), W);
	if (!FFileHelper::SaveStringToFile(OutPkg, *PkgPath))
	{
		OutWhy = TEXT("Could not write package.json into the new project.");
		return false;
	}

	return true;
}

static bool CopyTemplate(const FString& Dest, FString& OutWhy)
{
	if (GTemplateDir.IsEmpty())
	{
		OutWhy = TEXT("The Portal scripting template is not installed where the tool can find it. Install the Portal SDK, or set [BF6UnrealSDK] ScriptTemplateDir.");
		return false;
	}

	IFileManager& FM = IFileManager::Get();
	TArray<FString> Files;
	FM.FindFilesRecursive(Files, *GTemplateDir, TEXT("*"), true, false);

	int32 Copied = 0;
	for (const FString& Src : Files)
	{
		FString Rel = Src;
		FPaths::MakePathRelativeTo(Rel, *(GTemplateDir / TEXT("")));
		Rel.ReplaceInline(TEXT("\\"), TEXT("/"));
		if (ShouldSkipTemplateFile(Rel)) continue;

		const FString Dst = FPaths::Combine(Dest, Rel);
		FM.MakeDirectory(*FPaths::GetPath(Dst), true);
		if (FM.Copy(*Dst, *Src) == COPY_OK) ++Copied;
	}

	if (Copied == 0)
	{
		OutWhy = FString::Printf(TEXT("Nothing was copied out of %s."), *GTemplateDir);
		return false;
	}
	UE_LOG(LogBF6Script, Display, TEXT("Copied %d template files to %s"), Copied, *Dest);
	return true;
}

// ---- BF6Project ------------------------------------------------------------
// The two calls above, handed out so every save can be scaffolded through the
// SAME code the NEW PROJECT button uses. A second copy of the template rules
// would drift the moment the template moves, and a project made by the save
// path has to be indistinguishable from one made by npm run init.
FString BF6Script::TemplateDir()
{
	if (GTemplateDir.IsEmpty()) FindTemplate();
	return GTemplateDir;
}

bool BF6Script::ScaffoldProject(const FString& Dir, const FString& ExperienceName,
	const FString& Description, const FString& Boilerplate,
	const FString& ExperienceId, FString& OutWhy)
{
	if (Dir.IsEmpty()) { OutWhy = TEXT("No project folder was given."); return false; }
	if (!CopyTemplate(Dir, OutWhy)) return false;
	return ApplyInit(Dir, ExperienceName, Description,
		Boilerplate.IsEmpty() ? TEXT("plain") : Boilerplate, ExperienceId, OutWhy);
}

bool BF6Script::EnsureTemplateProject(const FString& ExperienceId, const FString& ExperienceName,
	FString& OutDir, FString& OutWhy)
{
	// The experience's own project, made if this is the first time anything has
	// been written for it. The same call WriteProjectShared uses, so a script
	// lands beside the settings and rotation rather than somewhere new.
	const FString Save = BF6Project::EnsureExperience(ExperienceId, ExperienceName);
	if (Save.IsEmpty())
	{
		OutWhy = TEXT("Could not work out which project this experience belongs to.");
		return false;
	}
	OutDir = BF6Project::DirFor(Save);
	if (OutDir.IsEmpty())
	{
		OutWhy = FString::Printf(TEXT("The project folder for '%s' is not known."), *Save);
		return false;
	}
	// package.json is the marker for "there is a project here". Without this
	// check a re-import would copy the template over the top of a project
	// somebody has been working in.
	if (FPaths::FileExists(FPaths::Combine(OutDir, TEXT("package.json"))))
	{
		// BUT IT STILL HAS TO KNOW WHICH EXPERIENCE IT IS.
		//
		// The project is usually made a moment earlier by BF6Project, writing
		// the first file of the import - and that path scaffolds with no
		// experience id, so .env ends up with MOD_ID="". Returning here without
		// filling it in left every imported project reporting "experience: not
		// linked to one", so pushing it asked to be linked by hand every time.
		WriteEnvModId(OutDir, ExperienceId);
		return true;
	}

	if (!ScaffoldProject(OutDir, ExperienceName, FString(), TEXT("plain"), ExperienceId, OutWhy))
	{
		return false;
	}
	UE_LOG(LogBF6Script, Display,
		TEXT("Scripting template scaffolded for '%s' at %s"), *ExperienceName, *OutDir);
	return true;
}

// Defined below, beside the build that reads it: a project whose source really
// is this experience's, rather than the template's example.
static void MarkSourceAdopted(const FString& ProjectDir, const FString& How);

bool BF6Script::ConvertBlocksToTemplate(const FString& ExperienceId, const FString& ExperienceName,
	const FString& WorkspaceJsonPath, FString& OutWhy)
{
	if (!FPaths::FileExists(WorkspaceJsonPath))
	{
		OutWhy = FString::Printf(TEXT("There is no block workspace at %s."), *WorkspaceJsonPath);
		return false;
	}
	if (GNodeExe.IsEmpty()) FindNode();
	if (GNodeExe.IsEmpty())
	{
		OutWhy = TEXT("Node is not installed where the tool can find it, so a block workspace "
		              "cannot be converted. Install it from nodejs.org, or set [BF6UnrealSDK] NodeExe.");
		return false;
	}

	const FString Cli = FPaths::Combine(BF6Ext::ToolPluginDir(), TEXT("Resources"),
		TEXT("convert"), TEXT("cli.js"));
	if (!FPaths::FileExists(Cli))
	{
		OutWhy = FString::Printf(TEXT("The converter is missing: %s"), *Cli);
		return false;
	}

	FString Dir;
	if (!EnsureTemplateProject(ExperienceId, ExperienceName, Dir, OutWhy)) return false;

	const FString Args = FString::Printf(TEXT("\"%s\" blocks2template \"%s\" \"%s\""),
		*Cli, *WorkspaceJsonPath, *Dir);
	// The converted workspace IS this experience's source, so a project that
	// arrived as blocks builds normally from here on.
	MarkSourceAdopted(Dir, TEXT("converted from this experience's block workspace"));
	// The source IS the mod now, so the bundle is no longer the imported one.
	BF6Script::ClearBundleImported(Dir);
	UE_LOG(LogBF6Script, Display,
		TEXT("Converting the block workspace of '%s' into its TypeScript project..."),
		*ExperienceName);
	return StartRun(GNodeExe, Args, Dir, TEXT("convert blocks"), 0, OutWhy, FString(), FString());
}
// ---- end BF6Project ----

// ---------------------------------------------------------------------------
// The project's files, as the page lists them.
// ---------------------------------------------------------------------------
// WHICH FILE TO SHOW WHEN A PROJECT IS OPENED.
//
// The page opened src/index.ts whenever nothing else was active, which is right
// for a project somebody is writing and wrong for one that has just been
// imported: there, src/index.ts is the TEMPLATE'S 69-line example, and the
// user's actual script is the 307 KB bundle the site sent back. So opening the
// Script tab after an import showed the template's DebugTool sample and looked
// exactly like the script had not come in at all.
//
// Set by the import, cleared once it has been honoured, so it only ever
// overrides the page's own choice on the first look after an import.
static FString GPreferredOpen;

// ---------------------------------------------------------------------------
// SOUNDS AND EFFECTS, AS A LIST YOU CAN SEARCH.
//
// A sound in a Portal script is a RuntimeSpawn enum member:
//
//   const sfx = mod.SpawnObject(RuntimeSpawn_Common.SFX_Alarm, pos, rot) as mod.SFX;
//
// so swapping one is swapping that identifier. The names come from the SDK's
// own typings (bf6-portal-mod-types/runtime-spawn-enums) - 938 SFX and 318
// effects in Common alone, plus per-map effects - which is type data shipped
// with the SDK rather than anything read out of the game.
//
// PREVIEW HAS TWO PATHS, in this order:
//
//   1. the High Poly add-on, which reads the game itself. Authoritative, and
//      needs no recording of anything.
//   2. a soundboard folder of clips recorded from the game, when one is
//      configured. Everything the first path would give, minus the install.
//
// Without either the list still searches and applies; it just cannot be
// listened to, which is a smaller loss than it sounds - the names are
// descriptive and applying one is what the feature is for.
// ---------------------------------------------------------------------------

// Where the SDK's type definitions are for the open project: its own
// node_modules first, the installed template second.
static FString RuntimeSpawnDir(const FString& ProjectDir)
{
	const TCHAR* Rel = TEXT("node_modules/bf6-portal-mod-types/runtime-spawn-enums");
	if (!ProjectDir.IsEmpty())
	{
		const FString Own = FPaths::Combine(ProjectDir, Rel);
		if (FPaths::DirectoryExists(Own)) return Own;
	}
	const FString T = BF6Script::TemplateDir();
	if (!T.IsEmpty())
	{
		const FString Tpl = FPaths::Combine(T, Rel);
		if (FPaths::DirectoryExists(Tpl)) return Tpl;
	}
	return FString();
}

// One entry per SFX_/FX_ member, with the enum it belongs to so the page can
// write a reference that compiles.
static void GatherSpawnNames(const FString& ProjectDir, TArray<TSharedPtr<FJsonValue>>& Out)
{
	const FString Dir = RuntimeSpawnDir(ProjectDir);
	if (Dir.IsEmpty()) return;
	IFileManager& FM = IFileManager::Get();
	TArray<FString> Files;
	FM.FindFiles(Files, *(Dir / TEXT("*.d.ts")), true, false);
	for (const FString& F : Files)
	{
		FString Text;
		if (!FFileHelper::LoadFileToString(Text, *FPaths::Combine(Dir, F))) continue;
		TArray<FString> Lines;
		Text.ParseIntoArrayLines(Lines, false);
		FString EnumName;
		for (const FString& RawLine : Lines)
		{
			const FString L = RawLine.TrimStartAndEnd();
			if (L.StartsWith(TEXT("export enum ")))
			{
				EnumName = L.RightChop(12);
				EnumName.RemoveFromEnd(TEXT(" {"));
				EnumName = EnumName.TrimStartAndEnd();
				continue;
			}
			if (EnumName.IsEmpty()) continue;
			// A member line: a bare identifier, optionally with a trailing comma.
			FString Name = L;
			Name.RemoveFromEnd(TEXT(","));
			Name = Name.TrimStartAndEnd();
			const bool bSfx = Name.StartsWith(TEXT("SFX_"));
			const bool bFx  = Name.StartsWith(TEXT("FX_")) || Name.StartsWith(TEXT("VFX_"));
			if (!bSfx && !bFx) continue;
			// Nothing with punctuation in it: that is a signature, not a member.
			if (Name.Contains(TEXT("(")) || Name.Contains(TEXT(":")) || Name.Contains(TEXT(" "))) continue;

			TSharedRef<FJsonObject> O = MakeShared<FJsonObject>();
			O->SetStringField(TEXT("name"), Name);
			O->SetStringField(TEXT("enum"), EnumName);
			O->SetStringField(TEXT("kind"), bSfx ? TEXT("sfx") : TEXT("fx"));
			Out.Add(MakeShared<FJsonValueObject>(O));
		}
	}
}

// The soundboard folder, when there is one: a manifest.json beside a sounds
// folder. Configured rather than guessed, because it is somebody's own
// recording of the game and the tool has no business assuming where it lives.
static FString SoundboardDir()
{
	FString Configured;
	if (GConfig)
	{
		GConfig->GetString(TEXT("BF6UnrealSDK"), TEXT("SoundboardDir"), Configured, GEngineIni);
	}
	if (!Configured.IsEmpty() && FPaths::FileExists(FPaths::Combine(Configured, TEXT("manifest.json"))))
	{
		return Configured;
	}

	// FOUND RATHER THAN ASKED FOR, when it is somewhere obvious. A soundboard
	// is a folder with a manifest.json and a sounds folder, and it usually sits
	// inside one of the SDK's own projects because that is where it was
	// recorded. Looked for once and remembered, since this walks directories.
	static bool bLooked = false;
	static FString Found;
	if (bLooked) return Found;
	bLooked = true;

	const FString Root = BF6Script::TemplateDir();
	if (!Root.IsEmpty())
	{
		// .../projects/_template-vX  ->  .../projects
		const FString Projects = FPaths::GetPath(Root);
		IFileManager& FM = IFileManager::Get();
		TArray<FString> Dirs;
		FM.FindFiles(Dirs, *(Projects / TEXT("*")), false, true);
		for (const FString& D : Dirs)
		{
			const FString Cand = FPaths::Combine(Projects, D, TEXT("tools"), TEXT("soundboard"));
			if (FPaths::FileExists(FPaths::Combine(Cand, TEXT("manifest.json"))))
			{
				Found = Cand;
				UE_LOG(LogBF6Script, Display,
					TEXT("Script: sound library found at %s. Set [BF6UnrealSDK] SoundboardDir to use another."),
					*Found);
				return Found;
			}
		}
	}
	return Found;
}

// Which of the two paths is available, best first.
//
// "addon" is the game itself: the High Poly add-on registers a placeable sound
// preview that resolves a placeable name to its sound EBX through
// bf6_sfx_placeable_resolve and decodes the real asset. That is the HQ answer,
// and it needs no recording of anything.
//
// "soundboard" is a folder of clips recorded from the game, for anyone without
// the add-on installed.
//
// Asked of the SOUND SYSTEM rather than of the plugin list, because an add-on
// that is present but has not opened an install yet cannot actually play
// anything, and a Play button that does nothing is worse than none.
static FString PreviewSource()
{
	// A name every install has, used only to ask "is this path alive at all".
	if (BF6UiSound::CanPreviewPlaceable(TEXT("SFX_Alarm"))) return TEXT("addon");
	if (!SoundboardDir().IsEmpty()) return TEXT("soundboard");
	return FString();
}

static void GatherProjectFiles(TArray<FString>& Out)
{
	if (GProjectDir.IsEmpty()) return;
	IFileManager& FM = IFileManager::Get();
	TArray<FString> Files;
	FM.FindFilesRecursive(Files, *FPaths::Combine(GProjectDir, TEXT("src")), TEXT("*"), true, false);
	for (const FString& F : Files)
	{
		const FString Ext = FPaths::GetExtension(F).ToLower();
		if (Ext != TEXT("ts") && Ext != TEXT("json")) continue;
		FString Rel = F;
		FPaths::MakePathRelativeTo(Rel, *(GProjectDir / TEXT("")));
		Rel.ReplaceInline(TEXT("\\"), TEXT("/"));
		Out.Add(Rel);
	}
	// The build output, so PULL FROM PORTAL and a look at what was built have
	// somewhere to land.
	for (const TCHAR* D : { TEXT("dist/bundle.ts"), TEXT("dist/bundle.strings.json") })
	{
		if (FPaths::FileExists(FPaths::Combine(GProjectDir, D))) Out.Add(D);
	}
	Out.Sort();
}

// ---------------------------------------------------------------------------
// WRITING A FILE SO IT CANNOT BE HALF WRITTEN, AND KEEPING WHAT IT WAS.
//
// The editor autosaves two seconds after the last keystroke, which means this
// runs often and runs while the user is still typing. Two rules follow:
//
//   write to a temporary beside the target and rename onto it, so a crash
//   between the two leaves the old file whole rather than a truncated one;
//
//   keep the last ten versions under <project>/.history, so "I broke it and
//   saved over it" is a folder away from being undone. The site signing
//   somebody out is only one of the ways work goes missing.
// ---------------------------------------------------------------------------
static void KeepHistory(const FString& Full, const FString& Rel)
{
	if (GProjectDir.IsEmpty() || !FPaths::FileExists(Full)) return;

	IFileManager& FM = IFileManager::Get();
	FString Flat = Rel;
	Flat.ReplaceInline(TEXT("/"), TEXT("__"));
	Flat.ReplaceInline(TEXT("\\"), TEXT("__"));

	const FString Dir = FPaths::Combine(GProjectDir, TEXT(".history"));
	FM.MakeDirectory(*Dir, true);

	// A stamp that sorts, so the oldest is the first in the list.
	const FString Stamp = FDateTime::Now().ToString(TEXT("%Y%m%d-%H%M%S"));
	FM.Copy(*FPaths::Combine(Dir, FString::Printf(TEXT("%s.%s"), *Flat, *Stamp)), *Full);

	TArray<FString> Old;
	FM.FindFiles(Old, *(Dir / (Flat + TEXT(".*"))), true, false);
	Old.Sort();
	for (int32 i = 0; i + 10 < Old.Num(); ++i)
	{
		FM.Delete(*FPaths::Combine(Dir, Old[i]));
	}
}

static bool WriteFileAtomic(const FString& Full, const FString& Text, FString& OutWhy)
{
	IFileManager& FM = IFileManager::Get();
	FM.MakeDirectory(*FPaths::GetPath(Full), true);

	const FString Temp = Full + TEXT(".bf6tmp");
	if (!FFileHelper::SaveStringToFile(Text, *Temp, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM))
	{
		OutWhy = FString::Printf(TEXT("Could not write %s."), *Temp);
		return false;
	}
	// DO NOT DELETE THE OLD ONE UNTIL THE NEW ONE IS IN PLACE.
	//
	// This deleted the destination and then moved the temporary over it. The
	// gap between those two calls is small and it is not empty: a virus
	// scanner holding the file, a permissions change, a full disk on the move
	// itself, and the destination is gone while the replacement is not there.
	// The name of the function promised the one thing it did not do.
	//
	// The old file is stepped aside instead, and put back if anything after
	// that fails. Worst case the user ends up with exactly what they had.
	const FString Backup = Full + TEXT(".bf6prev");
	const bool bHad = FPaths::FileExists(Full);
	if (bHad)
	{
		FM.Delete(*Backup, false, true, true);
		if (!FM.Move(*Backup, *Full, true, true))
		{
			// Could not even move the old one aside, so it is still the file on
			// disk and it stays that way. Nothing has been lost.
			FM.Delete(*Temp, false, true, true);
			OutWhy = FString::Printf(
				TEXT("Could not replace %s: the existing file could not be moved aside. ")
				TEXT("It is unchanged."), *Full);
			return false;
		}
	}

	if (!FM.Move(*Full, *Temp, true, true))
	{
		// Put back exactly what was there before.
		if (bHad) { FM.Move(*Full, *Backup, true, true); }
		FM.Delete(*Temp, false, true, true);
		OutWhy = FString::Printf(TEXT("Could not put %s in place.%s"), *Full,
			bHad ? TEXT(" The previous version has been restored.") : TEXT(""));
		return false;
	}

	if (bHad) { FM.Delete(*Backup, false, true, true); }
	return true;
}

// ---------------------------------------------------------------------------
// THE CHECKED BUILD.
//
// A BUNDLER EXITING ZERO IS NOT A GOOD BUILD. This is the whole of the reason
// this section exists, so it is written down rather than left to be
// rediscovered:
//
//   bf6-portal-bundler v1.4.1, given a source file with an unresolved import
//   and a type error, prints two warnings, DROPS the module it could not
//   resolve, writes dist/bundle.ts with "@ts-nocheck" at the top, and exits 0.
//
// Reproduced against the installed bundler in a real project. The tool used to
// read that exit code as "build finished", offer PUSH, and send a bundle whose
// program was not the program the user wrote. Portal accepts it, because
// @ts-nocheck tells its own compiler to stop looking.
//
// So BUILD is two stages and a proof:
//
//   1. TYPE CHECK. The project's own TypeScript, against the project's own
//      tsconfig.json, with --noEmit. It exits non-zero on an error, which is a
//      verdict rather than a guess. Nothing is bundled until it passes.
//   2. BUNDLE. The template's own bundler, as before. Its exit code is now the
//      weakest of the checks, not the only one: its output is read for the two
//      warnings that mean the bundle is wrong, and the file it claims to have
//      written must actually be newer than the run that wrote it.
//   3. THE PROOF. A hash of every compiled input and a hash of the bundle. PUSH
//      recomputes both. Editing any input, or the bundle, makes the project not
//      ready to send until it is built again.
//
// Bundler warnings stay visible in the log and on the page either way. They are
// not allowed to stand in for the type check having passed.
// ---------------------------------------------------------------------------

// The files whose content decides what the bundle should be. package-lock.json
// is in here because the dependency versions are part of the answer: the same
// source against a different bundler is a different bundle.

// A tsconfig.json may EXTEND another one, and everything in the base decides
// how the source is compiled just as much as the file that names it. Leaving
// the base out of the manifest meant editing it changed the compile and nothing
// noticed: the build stayed certified against a configuration that no longer
// existed.
//
// The target is read out of the text rather than out of parsed JSON because
// tsconfig files are routinely written with comments, which are not JSON and
// would make the parse fail silently. Naming one file too many only makes the
// manifest stricter; naming one too few is the defect.
static bool ExtendsTargetOf(const FString& Text, FString& Out)
{
	const int32 At = Text.Find(TEXT("\"extends\""), ESearchCase::CaseSensitive);
	if (At == INDEX_NONE) return false;
	int32 i = At + 9;
	while (i < Text.Len() && Text[i] != TEXT(':')) ++i;
	while (i < Text.Len() && Text[i] != TEXT('"')) ++i;
	if (i >= Text.Len()) return false;
	const int32 Start = ++i;
	while (i < Text.Len() && Text[i] != TEXT('"')) ++i;
	if (i >= Text.Len()) return false;
	Out = Text.Mid(Start, i - Start);
	return !Out.IsEmpty();
}

static void AddExtendedTsConfigs(const FString& Dir, TArray<FString>& OutFull)
{
	FString Current = FPaths::ConvertRelativePathToFull(FPaths::Combine(Dir, TEXT("tsconfig.json")));
	// Every file seen is remembered and the walk is capped, so a config that
	// extends itself, directly or round a ring, stops instead of spinning.
	TSet<FString> Seen;
	Seen.Add(Current);
	for (int32 Hops = 0; Hops < 8; ++Hops)
	{
		FString Text;
		if (!FFileHelper::LoadFileToString(Text, *Current)) return;
		FString Extends;
		if (!ExtendsTargetOf(Text, Extends)) return;

		FString Next = Extends.StartsWith(TEXT("."))
			? FPaths::Combine(FPaths::GetPath(Current), Extends)
			// A bare name is a package, which TypeScript looks up in node_modules.
			: FPaths::Combine(Dir, TEXT("node_modules"), Extends);
		Next = FPaths::ConvertRelativePathToFull(Next);
		if (!FPaths::FileExists(Next) && !Next.EndsWith(TEXT(".json"))) Next += TEXT(".json");
		if (!FPaths::FileExists(Next) || Seen.Contains(Next)) return;
		Seen.Add(Next);
		OutFull.Add(Next);
		Current = Next;
	}
}

static void GatherCompiledInputs(const FString& Dir, TArray<FString>& OutFull)
{
	if (Dir.IsEmpty()) return;
	IFileManager::Get().FindFilesRecursive(OutFull, *FPaths::Combine(Dir, TEXT("src")), TEXT("*"), true, false);
	OutFull.RemoveAll([](const FString& F)
	{
		const FString Ext = FPaths::GetExtension(F).ToLower();
		return Ext != TEXT("ts") && Ext != TEXT("json");
	});
	for (const TCHAR* N : { TEXT("tsconfig.json"), TEXT("package.json"), TEXT("package-lock.json") })
	{
		const FString P = FPaths::Combine(Dir, N);
		if (FPaths::FileExists(P)) OutFull.Add(P);
	}
	AddExtendedTsConfigs(Dir, OutFull);
	// Sorted, so the same set of files always hashes to the same thing however
	// the file system happened to enumerate them.
	OutFull.Sort();
}

// FAIL CLOSED ON A FILE THAT CANNOT BE READ.
//
// An unreadable file used to contribute its path and no bytes, which is the
// same thing an empty file contributes: two different unreadable files agreed,
// and a locked or deleted input could be hashed into a proof nobody could
// check. It is now a failure with a reason, and every caller turns that into a
// refusal rather than a hash.
static bool HashOfFiles(const TArray<FString>& Full, FString& OutHash, FString& OutWhy)
{
	FMD5 Md5;
	for (const FString& F : Full)
	{
		// The path goes into the hash as well as the bytes, so renaming a file
		// is a change even when nothing inside it moved.
		FTCHARToUTF8 Utf8(*F);
		Md5.Update((const uint8*)Utf8.Get(), Utf8.Length());
		TArray<uint8> Bytes;
		if (!FFileHelper::LoadFileToArray(Bytes, *F))
		{
			OutWhy = FString::Printf(TEXT("%s could not be read"), *F);
			return false;
		}
		if (Bytes.Num() > 0)
		{
			Md5.Update(Bytes.GetData(), Bytes.Num());
		}
	}
	uint8 Digest[16];
	Md5.Final(Digest);
	OutHash = BytesToHex(Digest, 16);
	return true;
}

static FString HashProjectInputs(const FString& Dir, FString* OutWhy)
{
	TArray<FString> Files;
	GatherCompiledInputs(Dir, Files);
	if (Files.Num() == 0)
	{
		if (OutWhy) *OutWhy = TEXT("no source files were found in the project");
		return FString();
	}
	FString Hash, Why;
	if (!HashOfFiles(Files, Hash, Why))
	{
		if (OutWhy) *OutWhy = Why;
		return FString();
	}
	return Hash;
}

static FString HashOneFile(const FString& Full)
{
	if (!FPaths::FileExists(Full)) return FString();
	TArray<FString> One;
	One.Add(Full);
	FString Hash, Why;
	return HashOfFiles(One, Hash, Why) ? Hash : FString();
}

static FString BundlePathFor(const FString& Dir)
{
	return FPaths::Combine(Dir, TEXT("dist"), TEXT("bundle.ts"));
}

static FString StringsPathFor(const FString& Dir)
{
	return FPaths::Combine(Dir, TEXT("dist"), TEXT("bundle.strings.json"));
}

// THE STRINGS ARTIFACT IS PART OF WHAT A BUILD CERTIFIES.
//
// PUSH sends it beside the bundle, so a build that produced one is only still
// valid while that exact file is still there, and a build that produced none is
// only still valid while there is still none. Both states are recorded, so
// editing it, deleting it, or one appearing afterwards all read as a change.
// "none" cannot collide with a hash, which is always hexadecimal.
static bool HashStringsArtifact(const FString& Dir, FString& OutHash, FString& OutWhy)
{
	const FString P = StringsPathFor(Dir);
	if (!FPaths::FileExists(P)) { OutHash = TEXT("none"); return true; }
	TArray<FString> One;
	One.Add(P);
	FString Why;
	if (!HashOfFiles(One, OutHash, Why))
	{
		OutWhy = TEXT("dist/bundle.strings.json is there but could not be read, so what push would send cannot be checked");
		return false;
	}
	return true;
}

// The project's own TypeScript, not one from anywhere else. Empty when the
// project's dependencies are not installed.
static FString TypeScriptCompiler(const FString& Dir)
{
	const FString Tsc = FPaths::Combine(Dir, TEXT("node_modules"), TEXT("typescript"), TEXT("lib"), TEXT("tsc.js"));
	return FPaths::FileExists(Tsc) ? Tsc : FString();
}

// WHAT ACTUALLY PRODUCED THE VERDICT, beyond the project's own files. Replace
// node, or point the project at a different TypeScript, and the check that said
// the source was good was made by something that is no longer installed. Sizes
// rather than content hashes because tsc.js is megabytes and this is recomputed
// on every readiness check; a replaced compiler that is byte-for-byte the same
// size is a compiler that is the same compiler for this purpose.
static FString ToolchainId(const FString& Dir)
{
	IFileManager& FM = IFileManager::Get();
	const FString Tsc = TypeScriptCompiler(Dir);
	return FString::Printf(TEXT("node=%s:%lld|tsc=%s:%lld"),
		*GNodeExe, GNodeExe.IsEmpty() ? (int64)-1 : FM.FileSize(*GNodeExe),
		*Tsc, Tsc.IsEmpty() ? (int64)-1 : FM.FileSize(*Tsc));
}

// The two things the bundler says while exiting zero that mean the bundle it
// wrote is not the program on disk. Everything else it prints is a warning and
// stays a warning.
static bool BundlerDisownedTheOutput(const FString& Out, FString& OutWhy)
{
	if (Out.Contains(TEXT("Could not resolve import")))
	{
		OutWhy = TEXT("it could not resolve an import, so that module is missing from the bundle");
		return true;
	}
	if (Out.Contains(TEXT("TypeScript reported errors")))
	{
		OutWhy = TEXT("its own TypeScript pass reported errors");
		return true;
	}
	return false;
}

// THE ONE WAY A BUNDLE GETS BUILT. Every entry point - the BUILD button, the
// template-tools list, the console command and the toolbar - comes through
// here, so none of them can grow a second, unchecked path to the same output.
// HAS THE REAL SOURCE BEEN BROUGHT IN?
//
// Written by UseMySource, and by a blocks conversion, because both replace the
// template's example with something that actually is this experience. Its
// presence is what allows a normal build on a project that arrived as a bundle.
static const TCHAR* kAdoptedMarker = TEXT(".bf6-source-adopted");

static bool SourceWasAdopted(const FString& ProjectDir)
{
	return !ProjectDir.IsEmpty()
		&& FPaths::FileExists(FPaths::Combine(ProjectDir, kAdoptedMarker));
}

// WHICH BUNDLES CAME FROM AN IMPORT.
//
// Written by the import, and by nothing else. The gate below used to decide
// this by looking at the bundle, and every bundle looks the same: banner,
// @ts-nocheck, one SOURCE line per module. A project made from the template and
// built once produces exactly that, so it was refused its second build with a
// message about importing source it had never lost.
static const TCHAR* kImportedMarker = TEXT(".bf6-imported-bundle");

static bool BundleCameFromImport(const FString& ProjectDir)
{
	return !ProjectDir.IsEmpty()
		&& FPaths::FileExists(FPaths::Combine(ProjectDir, kImportedMarker));
}

void BF6Script::MarkBundleImported(const FString& ProjectDir, const FString& Experience)
{
	if (ProjectDir.IsEmpty()) { return; }
	FFileHelper::SaveStringToFile(
		FString::Printf(TEXT("%s%s%s%s"), *Experience, LINE_TERMINATOR,
			*FDateTime::Now().ToString(), LINE_TERMINATOR),
		*FPaths::Combine(ProjectDir, kImportedMarker),
		FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
}

// The bundle is the project's own output from here on, so the import mark no
// longer describes it. Called when the source has been adopted and when a
// checked build has replaced the bundle with one made from that source.
void BF6Script::ClearBundleImported(const FString& ProjectDir)
{
	if (ProjectDir.IsEmpty()) { return; }
	IFileManager::Get().Delete(*FPaths::Combine(ProjectDir, kImportedMarker), false, true, true);
}

static void MarkSourceAdopted(const FString& ProjectDir, const FString& How)
{
	if (ProjectDir.IsEmpty()) { return; }
	FFileHelper::SaveStringToFile(How + LINE_TERMINATOR,
		*FPaths::Combine(ProjectDir, kAdoptedMarker),
		FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
}

static bool StartCheckedBuild(int32 ReqId, FString& OutWhy)
{
	if (GProjectDir.IsEmpty())
	{
		OutWhy = TEXT("Open or create a project first.");
		return false;
	}

	// AN IMPORTED MOD IS NOT BUILT FROM THE TEMPLATE'S EXAMPLE.
	//
	// Importing an experience writes the site's bundle to dist/bundle.ts and
	// scaffolds a template around it, so the project holds TWO different
	// programs: the mod that came down, and the starter src/ the template
	// ships. BUILD compiles src/ - so pressing it on a freshly imported
	// experience would overwrite the imported mod with the template example and
	// report a successful build. The mod would be gone, and the only sign would
	// be that the bundle got smaller.
	//
	// So a project whose bundle came from an import and whose source has not
	// been adopted refuses to build, and says the one thing that fixes it.
	// BF6.Script.UseMySource brings the real source in; after that src/ IS the
	// mod and this never fires again.
	{
		const FString Bundle = FPaths::Combine(GProjectDir, TEXT("dist"), TEXT("bundle.ts"));
		int32 Modules = 0;
		// BOTH conditions, and the first one is now a fact rather than a guess:
		// this bundle was written by an import, and the source has not been
		// brought in since. An authored project has no import mark, so it
		// builds as often as its author likes.
		if (BundleCameFromImport(GProjectDir) && BF6Script::LooksBundled(Bundle, Modules)
			&& !SourceWasAdopted(GProjectDir))
		{
			OutWhy = FString::Printf(
				TEXT("This experience came in as a built file made from %d source file(s), and this project's ")
				TEXT("src/ is still the template's example. Building would replace the imported mod with that ")
				TEXT("example. Run BF6.Script.UseMySource to bring in the project it was built from, then build."),
				Modules);
			return false;
		}
	}
	if (GNodeExe.IsEmpty())
	{
		OutWhy = TEXT("Node is not installed, or not where the tool looks. Install it from nodejs.org (version 24 or newer), then reopen this tab.");
		return false;
	}
	if (GNpmCli.IsEmpty())
	{
		OutWhy = TEXT("npm was not found beside node. Reinstall Node from nodejs.org so npm comes with it.");
		return false;
	}

	// PUBLISH MODE IS NOT PREPARED HERE ANY MORE.
	//
	// It used to copy src minus the debug tool before the build and then log
	// that the bundle was made from that copy, which was not true: the build
	// compiled the project's own tree and the push sent the ordinary output.
	// The copy is also the wrong unit of work, because the bundler decides what
	// is in a bundle by following imports.
	//
	// So it happens after the bundler has run, to the bundle itself, where the
	// content actually is and where the removal can be proved safe. See the
	// MakeLeanBundle call in RunTick.
	if (BF6Script::PublishMode(GProjectDir))
	{
		UE_LOG(LogBF6Script, Display,
			TEXT("Publish mode is on: the workbench code will be taken out of the built bundle, "
			     "and only if nothing in the mod uses it."));
	}

	const FString Tsc = TypeScriptCompiler(GProjectDir);
	if (Tsc.IsEmpty())
	{
		// Refusing is the point. Building without the type check is the defect
		// this whole section exists to close, so "check it if you happen to have
		// a compiler" would put the hole straight back.
		OutWhy = TEXT("This project's dependencies are not installed, so its TypeScript compiler is not there and the source cannot be checked. Press INSTALL first. Nothing is built without a type check.");
		return false;
	}
	const FString TsConfig = FPaths::Combine(GProjectDir, TEXT("tsconfig.json"));
	if (!FPaths::FileExists(TsConfig))
	{
		OutWhy = TEXT("This project has no tsconfig.json, so there is nothing to check the source against. Make the project again from the template.");
		return false;
	}

	// THE MANIFEST IS TAKEN BEFORE THE COMPILER IS STARTED, NOT AFTER IT FINISHES.
	//
	// Taken only at the end, it described whatever the files had become by then,
	// which is not necessarily what was compiled: edit a source file while tsc
	// or the bundler is reading it and the build was certified against an input
	// it never saw. RunTick confirms this same value at the end, and a build
	// whose inputs moved under it fails instead of certifying them.
	FString InputsWhy;
	const FString InputsAtStart = HashProjectInputs(GProjectDir, &InputsWhy);
	if (InputsAtStart.IsEmpty())
	{
		OutWhy = FString::Printf(
			TEXT("The project's files could not all be read, so there is nothing that can be checked: %s. ")
			TEXT("Close whatever has them open and press BUILD again."), *InputsWhy);
		return false;
	}

	// --pretty false keeps the terminal colour codes out of the log and the page.
	const FString CheckArgs = FString::Printf(TEXT("\"%s\" --noEmit --pretty false -p \"%s\""), *Tsc, *TsConfig);
	const FString BundleArgs = FString::Printf(TEXT("\"%s\" run build"), *GNpmCli);
	if (!StartRun(GNodeExe, CheckArgs, GProjectDir, TEXT("typecheck"), ReqId, OutWhy,
		TEXT("build"), BundleArgs))
	{
		return false;
	}
	if (GRun.IsValid()) GRun->InputsAtStart = InputsAtStart;
	return true;
}

// IS WHAT IS ON DISK SAFE TO SEND?
//
// Only when a checked build produced it AND nothing has been edited since. The
// hashes are recomputed here rather than trusted from a flag, because the files
// are the user's and the editor is not the only thing that writes them.
static bool ReadyToSend(FString& OutWhy)
{
	const FString BundlePath = BundlePathFor(GProjectDir);
	if (!FPaths::FileExists(BundlePath))
	{
		OutWhy = TEXT("There is no dist/bundle.ts yet. Press BUILD first.");
		return false;
	}
	// Every part of the proof, including the parts added after the first version
	// of this check shipped. A record missing any of them was written by an older
	// build and is not evidence about what is on disk now, so it is refused
	// rather than half trusted.
	if (GCheckedInputs.IsEmpty() || GCheckedBundle.IsEmpty()
		|| GCheckedStrings.IsEmpty() || GCheckedToolchain.IsEmpty())
	{
		OutWhy = TEXT("This bundle did not come from a checked build. Press BUILD, which type checks the source before bundling, then push.");
		return false;
	}
	if (HashOneFile(BundlePath) != GCheckedBundle)
	{
		OutWhy = TEXT("dist/bundle.ts has changed since the last checked build, so what is there is not what was checked. Press BUILD again, then push.");
		return false;
	}
	FString NowWhy;
	const FString InputsNow = HashProjectInputs(GProjectDir, &NowWhy);
	if (InputsNow.IsEmpty())
	{
		OutWhy = FString::Printf(
			TEXT("The project's files could not all be read, so the tool cannot tell whether the bundle is still current: %s. ")
			TEXT("Close whatever has them open, then press BUILD again."), *NowWhy);
		return false;
	}
	if (InputsNow != GCheckedInputs)
	{
		OutWhy = TEXT("The project's files have changed since the last checked build, so the bundle is out of date. Press BUILD again, then push.");
		return false;
	}
	// PUSH SENDS TWO FILES. Checking only one of them left the other free to
	// change after the build and still be sent as though it had been checked.
	FString StringsNow, StringsWhy;
	if (!HashStringsArtifact(GProjectDir, StringsNow, StringsWhy))
	{
		OutWhy = FString::Printf(TEXT("%s. Press BUILD again, then push."), *StringsWhy);
		return false;
	}
	if (StringsNow != GCheckedStrings)
	{
		OutWhy = (GCheckedStrings == TEXT("none"))
			? TEXT("dist/bundle.strings.json has appeared since the last checked build, and that build did not produce it, so it has not been checked. Press BUILD again, then push.")
			: (StringsNow == TEXT("none")
				? TEXT("dist/bundle.strings.json is gone, and the last checked build produced one, so push would send the bundle without its strings. Press BUILD again, then push.")
				: TEXT("dist/bundle.strings.json has changed since the last checked build, so what is there is not what was checked. Press BUILD again, then push."));
		return false;
	}
	if (ToolchainId(GProjectDir) != GCheckedToolchain)
	{
		OutWhy = TEXT("Node or this project's TypeScript compiler has changed since the last checked build, so the check was made by something that is not installed now. Press BUILD again, then push.");
		return false;
	}
	return true;
}

// ---------------------------------------------------------------------------
// THE PENDING BUNDLE.
//
// "Built at 14:32, not yet saved on Portal" is a fact about the project, not
// about this editor session, so it lives in the project. An editor restart, a
// crash, or the site signing the user out all leave it standing, and the page
// offers PUSH AGAIN the moment the panel is back on the right Script page.
// ---------------------------------------------------------------------------
static FString PendingPath()
{
	return GProjectDir.IsEmpty() ? FString() : FPaths::Combine(GProjectDir, TEXT(".bf6-pending.json"));
}

static void WritePending(const FString& State, const FString& Detail)
{
	const FString Path = PendingPath();
	if (Path.IsEmpty()) return;
	TSharedRef<FJsonObject> O = MakeShared<FJsonObject>();
	O->SetStringField(TEXT("state"), State);          // built | pushed | saved
	O->SetStringField(TEXT("detail"), Detail);
	O->SetStringField(TEXT("at"), FDateTime::Now().ToString(TEXT("%Y-%m-%d %H:%M:%S")));
	O->SetStringField(TEXT("experience"), GExperienceId);
	// The build proof rides along, so closing the editor between BUILD and PUSH
	// does not turn a checked build into an unchecked one. An empty proof is
	// written as empty rather than left out, so a stale one cannot survive a
	// build that failed its checks.
	O->SetStringField(TEXT("checkedInputs"), GCheckedInputs);
	O->SetStringField(TEXT("checkedBundle"), GCheckedBundle);
	O->SetStringField(TEXT("checkedStrings"), GCheckedStrings);
	O->SetStringField(TEXT("checkedToolchain"), GCheckedToolchain);
	O->SetStringField(TEXT("checkedAt"), GCheckedAt);
	// Which open this record was written under. It is only ever read back
	// alongside the directory it lives in, so it does not decide anything on its
	// own; it is here so a log or a support question can say which session's
	// build a pending state came from.
	O->SetNumberField(TEXT("projectGen"), (double)GProjectGen);
	FString Why;
	WriteFileAtomic(Path, JsonOf(O), Why);
}

static TSharedPtr<FJsonObject> ReadPending()
{
	const FString Path = PendingPath();
	FString Text;
	if (Path.IsEmpty() || !FFileHelper::LoadFileToString(Text, *Path)) return nullptr;
	return ParseJson(Text);
}

static void SendPending()
{
	TSharedPtr<FJsonObject> P = ReadPending();
	TSharedRef<FJsonObject> O = MakeShared<FJsonObject>();
	O->SetStringField(TEXT("kind"), TEXT("pending"));
	if (P.IsValid())
	{
		FString State, At, Detail;
		P->TryGetStringField(TEXT("state"), State);
		P->TryGetStringField(TEXT("at"), At);
		P->TryGetStringField(TEXT("detail"), Detail);
		O->SetStringField(TEXT("state"), State);
		O->SetStringField(TEXT("at"), At);
		O->SetStringField(TEXT("detail"), Detail);
	}
	else
	{
		O->SetStringField(TEXT("state"), TEXT("none"));
	}
	Event(O);
}

// ---------------------------------------------------------------------------
// THE LINE MAP.
//
// The site complains about a line in the BUNDLE. The user is looking at their
// own files. Without a map between the two, "line 4,812" is a number and
// nothing else.
//
// WHICH METHOD WORKS. Two were considered:
//
//   marker comments   insert a comment naming each file before bundling. The
//                     bundler is somebody else's and its treatment of comments
//                     is not ours to rely on; worse, it would mean building
//                     something other than what the user wrote. Rejected.
//
//   content spans     find each source file's own text inside the bundle and
//                     record where it landed. This is what is implemented. It
//                     needs nothing of the bundler, survives it changing, and
//                     it is checkable: a file whose span was not found is
//                     reported as unmapped rather than guessed at.
//
// The anchor for a file is the longest line in it that is long enough to be
// unique and is not an import (imports are rewritten by the bundler). From
// that anchor the file's span runs to the next file's anchor, and a bundle
// line inside a span maps to the source line at the same offset from the
// anchor. That is exact wherever the bundler copied the file through
// unchanged, which is the ordinary case, and it degrades to naming the right
// FILE rather than the right line where the bundler rewrote something.
// ---------------------------------------------------------------------------
namespace
{
	struct FSpan
	{
		FString Rel;
		int32   BundleStart = 0;   // 1 based
		int32   BundleEnd = 0;
		int32   SrcAnchor = 0;     // the source line the anchor is on
		int32   BundleAnchor = 0;
	};
	TArray<FSpan> GLineMap;
	FString       GLineMapBundle;
}

static bool AnchorFor(const TArray<FString>& Lines, FString& OutAnchor, int32& OutLine)
{
	int32 Best = -1, BestLen = 24;   // shorter than this is not distinctive
	for (int32 i = 0; i < Lines.Num(); ++i)
	{
		const FString T = Lines[i].TrimStartAndEnd();
		if (T.StartsWith(TEXT("import ")) || T.StartsWith(TEXT("export ")) ) continue;
		if (T.StartsWith(TEXT("//")) || T.StartsWith(TEXT("*")) || T.StartsWith(TEXT("/*"))) continue;
		if (T.Len() > BestLen) { BestLen = T.Len(); Best = i; }
	}
	if (Best < 0) return false;
	OutAnchor = Lines[Best].TrimStartAndEnd();
	OutLine = Best + 1;
	return true;
}

static int32 BuildLineMap()
{
	GLineMap.Reset();
	GLineMapBundle.Reset();
	if (GProjectDir.IsEmpty()) return 0;

	FString Bundle;
	if (!FFileHelper::LoadFileToString(Bundle, *FPaths::Combine(GProjectDir, TEXT("dist"), TEXT("bundle.ts")))) return 0;
	GLineMapBundle = Bundle;

	TArray<FString> BundleLines;
	Bundle.ParseIntoArrayLines(BundleLines, false);

	TArray<FString> Rel;
	GatherProjectFiles(Rel);

	for (const FString& R : Rel)
	{
		if (!R.StartsWith(TEXT("src/")) || !R.EndsWith(TEXT(".ts"))) continue;
		FString Text;
		if (!FFileHelper::LoadFileToString(Text, *FPaths::Combine(GProjectDir, R))) continue;

		TArray<FString> Lines;
		Text.ParseIntoArrayLines(Lines, false);

		FString Anchor; int32 SrcLine = 0;
		if (!AnchorFor(Lines, Anchor, SrcLine))
		{
			UE_LOG(LogBF6Script, Verbose, TEXT("Line map: %s has no line distinctive enough to anchor on."), *R);
			continue;
		}

		int32 Found = -1;
		for (int32 i = 0; i < BundleLines.Num(); ++i)
		{
			if (BundleLines[i].TrimStartAndEnd() == Anchor) { Found = i; break; }
		}
		if (Found < 0)
		{
			UE_LOG(LogBF6Script, Verbose, TEXT("Line map: %s was not found in the bundle; the bundler rewrote it."), *R);
			continue;
		}

		FSpan S;
		S.Rel = R;
		S.SrcAnchor = SrcLine;
		S.BundleAnchor = Found + 1;
		S.BundleStart = FMath::Max(1, S.BundleAnchor - (SrcLine - 1));
		S.BundleEnd = S.BundleStart + Lines.Num() - 1;
		GLineMap.Add(S);
	}

	GLineMap.Sort([](const FSpan& A, const FSpan& B) { return A.BundleStart < B.BundleStart; });
	UE_LOG(LogBF6Script, Display, TEXT("Line map: %d of the project's source files located in the bundle."), GLineMap.Num());
	return GLineMap.Num();
}

static bool MapBundleLine(int32 BundleLine, FString& OutRel, int32& OutSrcLine)
{
	for (const FSpan& S : GLineMap)
	{
		if (BundleLine < S.BundleStart || BundleLine > S.BundleEnd) continue;
		OutRel = S.Rel;
		OutSrcLine = FMath::Max(1, S.SrcAnchor + (BundleLine - S.BundleAnchor));
		return true;
	}
	return false;
}

// ---------------------------------------------------------------------------
// The type definitions.
//
// Read from the PROJECT's own node_modules, not from anywhere central, so the
// completion the user sees is the version their build will actually use.
// ---------------------------------------------------------------------------

// ---------------------------------------------------------------------------
// THREE TYPE SETS, RANKED.
//
// Three descriptions of the same API live on this machine and they are not the
// same size:
//
//   1. node_modules/bf6-portal-mod-types  the npm package the project builds
//      against. The STALEST of the three, and the only one with real JSDoc,
//      which is why it stays the base: the explanation gutter and the hover
//      text are written in it.
//   2. <sdk>/BF6_Frostbite_Research/data/mod.index.d.ts  read out of the
//      shipped game data. The RICHEST: a strict superset of the SDK's own by
//      function name.
//   3. <sdk>/code/types/mod/index.d.ts  the installed SDK's own.
//
// The editor used to load only the first, so completion did not know WaitUntil
// or SetObjectiveUIEnabled existed and a creator who typed them got a red line
// under working code.
//
// What is generated here is an OVERLAY: the declarations the base does not
// have, in name order of richness, wrapped in a second `declare namespace mod`
// that TypeScript merges with the first.
//
// FILLED IN BY NAME, NEVER BY SIGNATURE. If the base already declares Wait,
// the richer file's Wait is dropped whole. Adding it as an overload would let
// overload resolution pick the poorer shape and quietly break `await
// mod.Wait(1)`, which is a worse outcome than a missing entry.
// ---------------------------------------------------------------------------
struct FDeclNames
{
	TSet<FString> Fn, En, Ty, Co;
};

static FString BF6_Ident(const FString& Trimmed, const TCHAR* Keyword)
{
	const FString Kw(Keyword);
	if (!Trimmed.StartsWith(Kw)) return FString();
	int32 i = Kw.Len();
	while (i < Trimmed.Len() && FChar::IsWhitespace(Trimmed[i])) ++i;
	const int32 Start = i;
	while (i < Trimmed.Len() && (FChar::IsAlnum(Trimmed[i]) || Trimmed[i] == TEXT('_') || Trimmed[i] == TEXT('$'))) ++i;
	return (i > Start) ? Trimmed.Mid(Start, i - Start) : FString();
}

// Which names a piece of .d.ts declares. One level of nested namespace is
// tracked, because the event handler signatures live in one.
static void BF6_IndexDecls(const FString& Text, FDeclNames& Out)
{
	TArray<FString> Lines;
	Text.ParseIntoArrayLines(Lines, false);
	FString Ns;
	for (const FString& Raw : Lines)
	{
		const FString T = Raw.TrimStartAndEnd();
		FString N;
		if (!(N = BF6_Ident(T, TEXT("namespace"))).IsEmpty()) { Ns = N; continue; }
		if (!(N = BF6_Ident(T, TEXT("export namespace"))).IsEmpty()) { Ns = N; continue; }
		if (T == TEXT("}") && !Ns.IsEmpty()) { Ns.Reset(); continue; }
		const FString Pre = Ns.IsEmpty() ? FString() : (Ns + TEXT("."));
		if (!(N = BF6_Ident(T, TEXT("export function"))).IsEmpty())   { Out.Fn.Add(Pre + N); continue; }
		if (!(N = BF6_Ident(T, TEXT("export enum"))).IsEmpty())       { Out.En.Add(Pre + N); continue; }
		if (!(N = BF6_Ident(T, TEXT("enum"))).IsEmpty())              { Out.En.Add(Pre + N); continue; }
		if (!(N = BF6_Ident(T, TEXT("export type"))).IsEmpty())       { Out.Ty.Add(Pre + N); continue; }
		if (!(N = BF6_Ident(T, TEXT("export const"))).IsEmpty())      { Out.Co.Add(Pre + N); continue; }
		if (!(N = BF6_Ident(T, TEXT("const"))).IsEmpty())             { Out.Co.Add(Pre + N); continue; }
	}
}

// Everything in Text whose NAME the base does not already have, as source
// ready to drop inside a `declare namespace mod`. Base grows as it goes, so a
// second source cannot repeat what the first just contributed.
static FString BF6_MissingDecls(const FString& Text, FDeclNames& Base, int32& OutFn, int32& OutRest)
{
	TArray<FString> Lines;
	Text.ParseIntoArrayLines(Lines, false);

	TArray<FString> Top;
	TMap<FString, TArray<FString>> Nested;
	FString Ns;

	for (int32 i = 0; i < Lines.Num(); ++i)
	{
		const FString& Raw = Lines[i];
		const FString T = Raw.TrimStartAndEnd();
		FString N;

		if (!(N = BF6_Ident(T, TEXT("namespace"))).IsEmpty() && T.EndsWith(TEXT("{"))) { Ns = N; continue; }
		if (!(N = BF6_Ident(T, TEXT("export namespace"))).IsEmpty() && T.EndsWith(TEXT("{"))) { Ns = N; continue; }
		if (T == TEXT("}") && !Ns.IsEmpty()) { Ns.Reset(); continue; }

		const FString Pre = Ns.IsEmpty() ? FString() : (Ns + TEXT("."));
		TArray<FString>& Sink = Ns.IsEmpty() ? Top : Nested.FindOrAdd(Ns);

		// an enum, whole
		if (!(N = BF6_Ident(T, TEXT("export enum"))).IsEmpty())
		{
			int32 j = i;
			while (j < Lines.Num() && Lines[j].TrimStartAndEnd() != TEXT("}")) ++j;
			if (!Base.En.Contains(Pre + N))
			{
				for (int32 k = i; k <= j && k < Lines.Num(); ++k) Sink.Add(Lines[k]);
				Base.En.Add(Pre + N);
				++OutRest;
			}
			i = j;
			continue;
		}

		// a type alias, with the opaque symbol const that goes with it
		if (!(N = BF6_Ident(T, TEXT("export type"))).IsEmpty())
		{
			if (!Base.Ty.Contains(Pre + N))
			{
				if (i > 0)
				{
					const FString Prev = Lines[i - 1].TrimStartAndEnd();
					FString PN = BF6_Ident(Prev, TEXT("const"));
					if (PN.IsEmpty()) PN = BF6_Ident(Prev, TEXT("export const"));
					if (!PN.IsEmpty() && !Base.Co.Contains(Pre + PN))
					{
						Sink.Add(Lines[i - 1]);
						Base.Co.Add(Pre + PN);
						++OutRest;
					}
				}
				Sink.Add(Raw);
				Base.Ty.Add(Pre + N);
				++OutRest;
			}
			continue;
		}

		// a function. The generator wraps long parameter lists, so a
		// declaration runs until its semicolon.
		if (!(N = BF6_Ident(T, TEXT("export function"))).IsEmpty())
		{
			int32 j = i;
			while (j < Lines.Num() && !Lines[j].Contains(TEXT(";"))) ++j;
			if (!Base.Fn.Contains(Pre + N))
			{
				for (int32 k = i; k <= j && k < Lines.Num(); ++k) Sink.Add(Lines[k]);
				Base.Fn.Add(Pre + N);
				++OutFn;
			}
			i = j;
			continue;
		}

		if (!(N = BF6_Ident(T, TEXT("export const"))).IsEmpty())
		{
			if (!Base.Co.Contains(Pre + N)) { Sink.Add(Raw); Base.Co.Add(Pre + N); ++OutRest; }
			continue;
		}
	}

	FString Out;
	for (const FString& L : Top) Out += L + TEXT("\n");
	for (const TPair<FString, TArray<FString>>& P : Nested)
	{
		if (P.Value.Num() == 0) continue;
		Out += FString::Printf(TEXT("    namespace %s {\n"), *P.Key);
		for (const FString& L : P.Value) Out += L + TEXT("\n");
		Out += TEXT("    }\n");
	}
	return Out;
}

// Every PortalSDK root on the machine's fixed drives, newest last.
static void BF6_SdkRoots(TArray<FString>& Out)
{
	for (TCHAR Drive = TEXT('C'); Drive <= TEXT('F'); ++Drive)
	{
		const FString Base = FString::Printf(TEXT("%c:/"), Drive);
		if (!FPaths::DirectoryExists(Base)) continue;
		TArray<FString> Found;
		IFileManager::Get().FindFiles(Found, *(Base + TEXT("PortalSDK*")), false, true);
		Found.Sort();
		for (const FString& F : Found) Out.Add(Base + F);
	}
}

// The newest copy of one of the two rich type files, or an ini override.
static FString BF6_FindRichTypes(const TCHAR* RelPath, const TCHAR* IniKey)
{
	FString Configured;
	if (GConfig) GConfig->GetString(TEXT("BF6UnrealSDK"), IniKey, Configured, GEngineIni);
	if (!Configured.IsEmpty() && FPaths::FileExists(Configured)) return Configured;

	TArray<FString> Roots;
	BF6_SdkRoots(Roots);
	FString Best;
	for (const FString& R : Roots)
	{
		const FString Full = FPaths::Combine(R, RelPath);
		if (FPaths::FileExists(Full) && Full > Best) Best = Full;
	}
	return Best;
}

// What the last type load found, so the page can say it out loud.
static TArray<TSharedPtr<FJsonValue>> GTypeSources;

// The overlay has to reach the COMPILER too, or completion offers calls that
// then fail to build, which is worse than not offering them. It goes in as a
// file under the project with one line added to tsconfig's include, which is
// the smallest change the template tolerates.
static void WriteTypeOverlay(const FString& Text)
{
	if (GProjectDir.IsEmpty()) return;

	const FString Dir = FPaths::Combine(GProjectDir, TEXT("types"));
	const FString File = FPaths::Combine(Dir, TEXT("bf6-mod-extra.d.ts"));
	FString Was;
	if (!FFileHelper::LoadFileToString(Was, *File) || Was != Text)
	{
		IFileManager::Get().MakeDirectory(*Dir, true);
		FFileHelper::SaveStringToFile(Text, *File, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
		UE_LOG(LogBF6Script, Display, TEXT("Wrote the type overlay to %s"), *File);
	}

	const FString TsConfig = FPaths::Combine(GProjectDir, TEXT("tsconfig.json"));
	FString Cfg;
	if (!FFileHelper::LoadFileToString(Cfg, *TsConfig)) return;
	if (Cfg.Contains(TEXT("types/**/*.d.ts"))) return;

	const FString Old(TEXT("\"include\": [\"src/**/*.ts\"]"));
	const FString New(TEXT("\"include\": [\"src/**/*.ts\", \"types/**/*.d.ts\"]"));
	if (!Cfg.Contains(Old))
	{
		UE_LOG(LogBF6Script, Warning,
			TEXT("tsconfig.json does not have the template's own include line, so the type overlay was not added to it. ")
			TEXT("Add \"types/**/*.d.ts\" to include by hand if the compiler disagrees with the editor."));
		return;
	}
	Cfg.ReplaceInline(*Old, *New);
	FFileHelper::SaveStringToFile(Cfg, *TsConfig, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
	UE_LOG(LogBF6Script, Display, TEXT("tsconfig.json now compiles the type overlay as well."));
}

static void AddTypeSource(const TCHAR* Name, const FString& Path, int32 Fn, int32 Rest)
{
	TSharedRef<FJsonObject> O = MakeShared<FJsonObject>();
	O->SetStringField(TEXT("name"), Name);
	O->SetStringField(TEXT("path"), Path);
	O->SetNumberField(TEXT("functions"), Fn);
	O->SetNumberField(TEXT("other"), Rest);
	GTypeSources.Add(MakeShared<FJsonValueObject>(O));
}

static int32 QueueTypeLibs()
{
	if (GProjectDir.IsEmpty()) return 0;
	IFileManager& FM = IFileManager::Get();
	const FString Modules = FPaths::Combine(GProjectDir, TEXT("node_modules"));
	if (!FPaths::DirectoryExists(Modules)) return 0;

	GTypeSources.Reset();
	FDeclNames Base;
	int32 BaseFn = 0;

	int32 Sent = 0;
	const TCHAR* Packages[] = { TEXT("bf6-portal-mod-types"), TEXT("bf6-portal-utils") };
	for (const TCHAR* Pkg : Packages)
	{
		const FString Root = FPaths::Combine(Modules, Pkg);
		if (!FPaths::DirectoryExists(Root)) continue;

		TArray<FString> Files;
		FM.FindFilesRecursive(Files, *Root, TEXT("*"), true, false);
		for (const FString& F : Files)
		{
			const FString Lower = F.ToLower();
			// .d.ts always. A plain .ts as well for bf6-portal-utils, because
			// its modules are shipped as source and that source carries the
			// JSDoc the explanation gutter reads.
			const bool bDts = Lower.EndsWith(TEXT(".d.ts"));
			const bool bTs = Lower.EndsWith(TEXT(".ts")) && !bDts;
			if (!bDts && !bTs) continue;
			if (Lower.Contains(TEXT("/test/")) || Lower.Contains(TEXT("\\test\\"))) continue;

			FString Text;
			if (!FFileHelper::LoadFileToString(Text, *F)) continue;
			if (Text.Len() > 2 * 1024 * 1024) continue;   // a generated monster is not documentation

			FString Rel = F;
			FPaths::MakePathRelativeTo(Rel, *(Modules / TEXT("")));
			Rel.ReplaceInline(TEXT("\\"), TEXT("/"));
			PushPayload(TEXT("lib"), Rel, Text);
			++Sent;

			// The base's own names, so the overlay knows what not to repeat.
			if (Lower.Contains(TEXT("bf6-portal-mod-types"))) BF6_IndexDecls(Text, Base);
		}
	}
	BaseFn = Base.Fn.Num();
	AddTypeSource(TEXT("bf6-portal-mod-types (this project)"), Modules, BaseFn,
		Base.En.Num() + Base.Ty.Num() + Base.Co.Num());

	// ---- the two richer sets, poorest last ---------------------------------
	FString Overlay;
	struct FRich { const TCHAR* Name; const TCHAR* Rel; const TCHAR* IniKey; };
	static const FRich kRich[] = {
		{ TEXT("game data (BF6_Frostbite_Research)"), TEXT("BF6_Frostbite_Research/data/mod.index.d.ts"), TEXT("ScriptTypesResearch") },
		{ TEXT("installed SDK (code/types/mod)"),     TEXT("code/types/mod/index.d.ts"),                  TEXT("ScriptTypesSdk") }
	};

	for (const FRich& R : kRich)
	{
		const FString Path = BF6_FindRichTypes(R.Rel, R.IniKey);
		if (Path.IsEmpty())
		{
			// A creator without the research repo still gets the npm types and
			// everything else. This is not a failure and must not read as one.
			UE_LOG(LogBF6Script, Display, TEXT("No %s type set on this machine; the editor uses what it has."), R.Name);
			AddTypeSource(R.Name, FString(), -1, -1);
			continue;
		}
		FString Text;
		if (!FFileHelper::LoadFileToString(Text, *Path))
		{
			AddTypeSource(R.Name, Path, -1, -1);
			continue;
		}
		int32 Fn = 0, Rest = 0;
		const FString Part = BF6_MissingDecls(Text, Base, Fn, Rest);
		AddTypeSource(R.Name, Path, Fn, Rest);
		if (!Part.IsEmpty())
		{
			Overlay += FString::Printf(TEXT("    // ---- filled in from the %s type set ----\n"), R.Name);
			Overlay += Part;
		}
	}

	if (!Overlay.IsEmpty())
	{
		const FString Full =
			TEXT("// Generated by the BF6 Unreal SDK tool. Do not edit.\n")
			TEXT("//\n")
			TEXT("// The parts of the mod API that this project's npm type package does not\n")
			TEXT("// describe, taken from the richer type sets on this machine so the editor\n")
			TEXT("// and the compiler agree about what exists.\n")
			TEXT("\ndeclare namespace mod {\n") + Overlay + TEXT("}\n");

		PushPayload(TEXT("lib"), TEXT("bf6-portal-mod-types/bf6-mod-extra.d.ts"), Full);
		++Sent;
		WriteTypeOverlay(Full);
	}

	UE_LOG(LogBF6Script, Display, TEXT("Queued %d type files from %s"), Sent, *Modules);
	return Sent;
}

// ---------------------------------------------------------------------------
// Monaco's language service worker.
//
// A worker cannot be constructed from a file:// URL, so the page builds one
// from a Blob instead, and the source for that Blob has to reach the page as
// TEXT. Pushing six megabytes across the bridge would work and would be slow,
// so it is written once as a plain .js file that assigns the source to a
// global, and the page loads THAT with a script tag: a file:// page may load a
// file:// script, it just may not fetch one.
//
// The file is regenerated when either input is newer than it, so a Monaco
// upgrade does not need anyone to remember this.
// ---------------------------------------------------------------------------
static FString EnsureWorkerSourceFile()
{
	const FString VendorVs = FPaths::Combine(ScriptRes(), TEXT("vendor"), TEXT("monaco"), TEXT("min"), TEXT("vs"));
	const FString MainJs = FPaths::Combine(VendorVs, TEXT("base"), TEXT("worker"), TEXT("workerMain.js"));
	const FString TsJs = FPaths::Combine(VendorVs, TEXT("language"), TEXT("typescript"), TEXT("tsWorker.js"));
	const FString OutJs = FPaths::Combine(GeneratedDir(), TEXT("monaco_worker_src.js"));

	if (!FPaths::FileExists(MainJs) || !FPaths::FileExists(TsJs))
	{
		UE_LOG(LogBF6Script, Warning,
			TEXT("Monaco's worker files are missing under %s. The editor still opens, in index mode: completion and hover work, live error checking does not."),
			*VendorVs);
		return FString();
	}

	IFileManager& FM = IFileManager::Get();
	const FDateTime OutTime = FM.GetTimeStamp(*OutJs);
	if (FPaths::FileExists(OutJs)
		&& OutTime > FM.GetTimeStamp(*MainJs)
		&& OutTime > FM.GetTimeStamp(*TsJs))
	{
		return OutJs;
	}

	FString Main, Ts;
	if (!FFileHelper::LoadFileToString(Main, *MainJs) || !FFileHelper::LoadFileToString(Ts, *TsJs))
	{
		UE_LOG(LogBF6Script, Warning, TEXT("Monaco's worker files could not be read."));
		return FString();
	}

	// workerMain installs the module loader and the message handler. tsWorker
	// declares itself by name, so inlining it straight after means the loader
	// finds the module already there and never reaches for importScripts,
	// which from a blob worker would be refused anyway.
	FString Src;
	Src.Reserve(Main.Len() + Ts.Len() + 512);
	Src += TEXT("/* Monaco language service worker, assembled by the BF6 Unreal SDK.\n");
	Src += TEXT("   Two of Monaco's own files, in the order the loader needs them.\n");
	Src += TEXT("   Monaco is MIT: see Resources/script/vendor/monaco/LICENSE.md. */\n");
	Src += TEXT("self.MonacoEnvironment = { baseUrl: '' };\n");
	Src += Main;
	Src += TEXT("\n;\n");
	Src += Ts;
	Src += TEXT("\n");

	FString File;
	File.Reserve(Src.Len() + Src.Len() / 8);
	File += TEXT("// Generated by the BF6 Unreal SDK. Do not edit; it is rebuilt when Monaco changes.\n");
	File += TEXT("window.__bf6WorkerSrc = ");
	File += JsQuote(Src);
	File += TEXT(";\n");

	FM.MakeDirectory(*GeneratedDir(), true);
	if (!FFileHelper::SaveStringToFile(File, *OutJs, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM))
	{
		UE_LOG(LogBF6Script, Warning, TEXT("Could not write %s."), *OutJs);
		return FString();
	}
	UE_LOG(LogBF6Script, Display, TEXT("Wrote the language service worker source: %s (%d chars)"), *OutJs, File.Len());
	return OutJs;
}

static FString FileUrl(const FString& Path)
{
	FString P = FPaths::ConvertRelativePathToFull(Path);
	P.ReplaceInline(TEXT("\\"), TEXT("/"));
	P.ReplaceInline(TEXT(" "), TEXT("%20"));
	return TEXT("file:///") + P;
}

// ---------------------------------------------------------------------------
// The snippets, off disk, so a user can drop their own beside ours.
// ---------------------------------------------------------------------------
static FString ReadSnippetPacks()
{
	IFileManager& FM = IFileManager::Get();
	const FString Dir = FPaths::Combine(ScriptRes(), TEXT("snippets"));
	TArray<FString> Files;
	FM.FindFiles(Files, *(Dir / TEXT("*.json")), true, false);

	TArray<FString> Packs;
	for (const FString& F : Files)
	{
		FString Text;
		if (FFileHelper::LoadFileToString(Text, *FPaths::Combine(Dir, F))) Packs.Add(Text);
	}
	return TEXT("[") + FString::Join(Packs, TEXT(",")) + TEXT("]");
}

// ---------------------------------------------------------------------------
// PortalLog, while the user hosts locally.
// ---------------------------------------------------------------------------
static FString PortalLogPath()
{
	// THE TRADEMARK SIGN IS NOT A TRADEMARK SIGN ON DISK.
	//
	// This used to build the folder as "Battlefield" + U+2122 + " 6", with a
	// comment explaining that constructing it kept the file plain ASCII. The
	// reasoning was sound and the result never matched anything: the name
	// Battlefield actually writes is the UTF-8 bytes for that character read
	// back as Windows-1252, so the folder on disk is literally
	// "Battlefielda<tm> 6" and stays that way.
	//
	// So WATCH THE GAME'S LOG has been reporting "that file does not exist yet"
	// for every user since it shipped, including immediately after a session
	// that wrote the file. BF6GameLog finds the folder by pattern and confirms
	// it by the log being inside it, which is the only reliable way.
	return BF6GameLog::LocateFolder().IsEmpty()
		? FString()
		: FPaths::Combine(BF6GameLog::LocateFolder(), TEXT("PortalLog.txt"));
}

static bool TailTick(float)
{
	if (!GTailing) { GTailTick.Reset(); return false; }

	IFileManager& FM = IFileManager::Get();
	const int64 Size = FM.FileSize(*GTailPath);
	if (Size < 0) return true;                 // not written yet: keep waiting
	if (Size < GTailOffset) GTailOffset = 0;   // the game restarted and truncated it
	if (Size == GTailOffset) return true;

	TUniquePtr<FArchive> Reader(FM.CreateFileReader(*GTailPath));
	if (!Reader) return true;

	const int64 Want = FMath::Min<int64>(Size - GTailOffset, 256 * 1024);
	Reader->Seek(GTailOffset);
	TArray<uint8> Bytes;
	Bytes.SetNumUninitialized((int32)Want + 1);
	Reader->Serialize(Bytes.GetData(), Want);
	Bytes[(int32)Want] = 0;
	GTailOffset += Want;
	Reader->Close();

	const FString Chunk = FString(UTF8_TO_TCHAR((const ANSICHAR*)Bytes.GetData()));
	TArray<FString> Lines;
	Chunk.ParseIntoArrayLines(Lines, false);

	TArray<TSharedPtr<FJsonValue>> Out;
	for (const FString& L : Lines)
	{
		if (L.TrimStartAndEnd().IsEmpty()) continue;
		Out.Add(MakeShared<FJsonValueString>(L));
	}
	if (Out.Num())
	{
		TSharedRef<FJsonObject> O = MakeShared<FJsonObject>();
		O->SetStringField(TEXT("kind"), TEXT("portallog"));
		O->SetArrayField(TEXT("lines"), Out);
		Event(O);
	}
	return true;
}

// ---------------------------------------------------------------------------
// The scene selection, so INSERT SELECTED OBJECT knows what to insert and a
// hover over a getter can name the object.
//
// Read through the tool's PUBLIC seams only: BF6Ext::Selection for what is
// selected and BF6Api::GatherObjIds for the script-facing id registry. Neither
// needs a line changed in a file this feature does not own.
// ---------------------------------------------------------------------------
static bool ObjIdForActor(const AActor* A, int32& OutId, FString& OutName, FString& OutType)
{
	if (!A) return false;
	const TArray<BF6Api::FObjIdRow> Rows = BF6Api::GatherObjIds();
	for (const BF6Api::FObjIdRow& R : Rows)
	{
		if (R.Actor.Get() != A) continue;
		OutId = R.Id; OutName = R.Name; OutType = R.Type;
		return R.Id >= 0;
	}
	return false;
}

static bool SelectionTick(float)
{
	if (!GPageReady) return true;

	TArray<AActor*> Sel;
	BF6Ext::Selection(Sel);

	int32 Id = -1; FString Name, Type;
	if (Sel.Num() == 1) ObjIdForActor(Sel[0], Id, Name, Type);

	if (Id == GLastSelObj) return true;
	GLastSelObj = Id;

	TSharedRef<FJsonObject> O = MakeShared<FJsonObject>();
	O->SetStringField(TEXT("kind"), TEXT("selection"));
	if (Id >= 0)
	{
		O->SetNumberField(TEXT("objid"), Id);
		O->SetStringField(TEXT("label"), Name);
		O->SetStringField(TEXT("kindName"), Type);
		O->SetStringField(TEXT("kind2"), Type);
	}
	Event(O);
	return true;
}

// ---------------------------------------------------------------------------
// HOW THE CREATOR LEFT THE WINDOW.
//
// The editor is one canvas with a strip of slide-out panels down its left edge,
// and which one was open is a preference, not a fact about the project: it
// belongs to the person, so it is kept per project in the editor ini beside
// every other panel state the tool remembers (SymbolsPinned and friends).
//
// Everything is a string. The page decides what "true" means for each key, and
// a key nobody wrote yet simply does not come back, which is how the page tells
// a first run from a return visit.
// ---------------------------------------------------------------------------
static const TCHAR* kPrefSection = TEXT("BF6UnrealSDK");
static const TCHAR* kScriptPrefKeys[] = {
	TEXT("shelf"),        // which handle was last open
	TEXT("shelfOpen"),    // whether it was open at all
	TEXT("shelfPinned"),  // whether it stays open while you type
	TEXT("console"),      // whether the message log was showing
	TEXT("guided")        // whether the plain-words gutter was on
};

static FString ScriptPrefIniKey(const FString& Name) { return TEXT("ScriptUi_") + Name; }

static bool IsKnownScriptPref(const FString& Name)
{
	for (const TCHAR* K : kScriptPrefKeys) if (Name == K) return true;
	return false;
}

static void AddScriptPrefs(const TSharedRef<FJsonObject>& O)
{
	TSharedRef<FJsonObject> P = MakeShared<FJsonObject>();
	for (const TCHAR* K : kScriptPrefKeys)
	{
		FString V;
		if (GConfig && GConfig->GetString(kPrefSection, *ScriptPrefIniKey(K), V, GEditorPerProjectIni))
		{
			P->SetStringField(K, V);
		}
	}
	O->SetObjectField(TEXT("prefs"), P);
}

// ---------------------------------------------------------------------------
// Status, as one object, sent whenever anything in it moves.
// ---------------------------------------------------------------------------
static void SendStatus(int32 ReplyTo = 0)
{
	TSharedRef<FJsonObject> O = MakeShared<FJsonObject>();
	O->SetStringField(TEXT("kind"), TEXT("status"));
	O->SetStringField(TEXT("node"), GNodeExe);
	O->SetStringField(TEXT("template"), GTemplateDir);
	AddScriptPrefs(O);
	if (!GProjectDir.IsEmpty())
	{
		TSharedRef<FJsonObject> P = MakeShared<FJsonObject>();
		P->SetStringField(TEXT("name"), GProjectName);
		P->SetStringField(TEXT("path"), GProjectDir);
		P->SetStringField(TEXT("experience"), GExperienceId);
		// The page stamps this back onto every request that means "in the
		// project I have open", and RequestNamesTheOpenProject refuses one that
		// quotes a generation this side has moved past. Status is also the
		// answer to `open`, so the page learns the new generation at exactly the
		// moment the switch it asked for happened.
		P->SetNumberField(TEXT("gen"), (double)GProjectGen);
		O->SetObjectField(TEXT("project"), P);
	}
	if (ReplyTo) { O->SetBoolField(TEXT("ok"), true); Reply(ReplyTo, O); }
	else Event(O);
}

// A project is LINKED when it lives at
//   Saved/BF6UnrealSDK/portal/experiences/<id>/script
// and its experience is that folder's name. The old test was "the folder is
// called script", which also matched a project the user happened to name
// "script" and would have handed back a nonsense id.
static FString ExperienceIdForProjectDir(const FString& Dir)
{
	if (Dir.IsEmpty() || FPaths::GetCleanFilename(Dir) != TEXT("script")) return FString();
	const FString ExpDir = FPaths::GetPath(Dir);
	const FString Experiences = FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("BF6UnrealSDK"),
		TEXT("portal"), TEXT("experiences"));
	if (FPaths::ConvertRelativePathToFull(FPaths::GetPath(ExpDir))
		!= FPaths::ConvertRelativePathToFull(Experiences)) return FString();
	return FPaths::GetCleanFilename(ExpDir);
}

static void OpenProject(const FString& Dir)
{
	GProjectDir = Dir;
	GProjectName = FPaths::GetCleanFilename(Dir);
	// EVERY open is a new generation, including reopening the same directory.
	// A request that left before this point was written for a project state
	// that has ended, and reopening the same folder does not make it current
	// again: the page's file table was rebuilt in between either way.
	++GProjectGen;

	// THE DESTINATION IS DERIVED FRESH FOR EVERY PROJECT, INCLUDING UNLINKED
	// ONES.
	//
	// This used only ever to SET the experience id, for a project living under
	// an experience folder, and never to clear it. Open a linked project and
	// then an unlinked one and the tool still believed it was aimed at the first
	// project's experience: PUSH from the unlinked project would then have
	// written its bundle into somebody else's experience and reported success.
	// A project with no destination now has an EMPTY one, and push refuses.
	GExperienceId = ExperienceIdForProjectDir(Dir);
	if (!GExperienceId.IsEmpty())
	{
		// A project living under an experience is named by its experience.
		GProjectName = GExperienceId;
	}
	else
	{
		// An unlinked project may still have been deliberately pointed at an
		// experience, by LINK TO PAGE or by whoever set up the project outside
		// the tool. That is written where the template keeps it.
		GExperienceId = ReadEnvModId(Dir);
	}

	// THE BUILD PROOF BELONGS TO THE PROJECT, NOT THE SESSION. Dropping it here
	// and reading back only this project's own is what stops the previous
	// project's checked build from making this one look ready to send.
	GCheckedInputs.Reset();
	GCheckedBundle.Reset();
	GCheckedStrings.Reset();
	GCheckedToolchain.Reset();
	GCheckedAt.Reset();
	if (TSharedPtr<FJsonObject> P = ReadPending())
	{
		P->TryGetStringField(TEXT("checkedInputs"), GCheckedInputs);
		P->TryGetStringField(TEXT("checkedBundle"), GCheckedBundle);
		// A record from before these were recorded leaves them empty, and
		// ReadyToSend refuses an incomplete proof rather than trusting the half
		// of it that happens to be there.
		P->TryGetStringField(TEXT("checkedStrings"), GCheckedStrings);
		P->TryGetStringField(TEXT("checkedToolchain"), GCheckedToolchain);
		P->TryGetStringField(TEXT("checkedAt"), GCheckedAt);
	}

	UE_LOG(LogBF6Script, Display, TEXT("Project open: %s (experience: %s)"), *GProjectDir,
		GExperienceId.IsEmpty() ? TEXT("not linked to one") : *GExperienceId);
}

// LINKING IS A DELIBERATE ACT, AND THE ONLY WAY A PROJECT GAINS A DESTINATION.
//
// The tool used to adopt whatever experience the Portal panel happened to be
// showing the moment an unlinked project was open, silently, and then treat
// that guess as the project's identity for every push afterwards. A guess is
// exactly what must not decide which experience gets written to, so the guess
// is gone and this is what replaces it: the user says so, once, about the page
// in front of them, and it is written into the project.
static bool LinkProjectToPage(FString& OutWhy)
{
	if (GProjectDir.IsEmpty())
	{
		OutWhy = TEXT("No script project is open.");
		return false;
	}
	if (!GSiteOnScriptPage || GSitePageExperience.IsEmpty())
	{
		OutWhy = TEXT("The Portal panel is not on an experience's Script page, so there is nothing to link this project to. Open the experience there first.");
		return false;
	}
	const FString FromPath = ExperienceIdForProjectDir(GProjectDir);
	if (!FromPath.IsEmpty() && !FromPath.Equals(GSitePageExperience, ESearchCase::IgnoreCase))
	{
		// This project lives inside one experience's own folder. Its identity is
		// where it is, and pointing it somewhere else would leave the folder
		// name and the destination disagreeing for ever after.
		OutWhy = FString::Printf(
			TEXT("This project is experience %s's own script project, so it cannot be pointed at %s. ")
			TEXT("Open %s's Script page instead, or make a separate project for %s."),
			*FromPath, *GSitePageExperience, *FromPath, *GSitePageExperience);
		return false;
	}

	const FString Was = GExperienceId;
	GExperienceId = GSitePageExperience;
	WriteEnvModId(GProjectDir, GExperienceId);
	UE_LOG(LogBF6Script, Display, TEXT("Project %s is now linked to experience %s (was %s)."),
		*GProjectName, *GExperienceId, Was.IsEmpty() ? TEXT("not linked") : *Was);
	return true;
}

static void ListProjects(TArray<FString>& Out)
{
	IFileManager& FM = IFileManager::Get();

	TArray<FString> Dirs;
	FM.FindFiles(Dirs, *(ScriptSaved() / TEXT("*")), false, true);
	for (const FString& D : Dirs)
	{
		if (D.StartsWith(TEXT("_"))) continue;   // _generated and friends
		const FString Full = FPaths::Combine(ScriptSaved(), D);
		if (FPaths::FileExists(FPaths::Combine(Full, TEXT("package.json")))) Out.Add(Full);
	}

	const FString Experiences = FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("BF6UnrealSDK"),
		TEXT("portal"), TEXT("experiences"));
	TArray<FString> Exp;
	FM.FindFiles(Exp, *(Experiences / TEXT("*")), false, true);
	for (const FString& E : Exp)
	{
		const FString Full = FPaths::Combine(Experiences, E, TEXT("script"));
		if (FPaths::FileExists(FPaths::Combine(Full, TEXT("package.json")))) Out.Add(Full);
	}

	// AND THE PROJECTS THAT BELONG TO A SAVED EXPERIENCE.
	//
	// This is where an imported experience's project actually lives - saves/
	// experiences/<name>, the one folder a creator is ever asked to look in -
	// and it was the one place this list did not look. So importing an
	// experience wrote its whole script into a project the panel could not even
	// offer: not in the dropdown, therefore not selectable, therefore the boot
	// fell back to whatever sorted first and the script looked like it had
	// never arrived.
	const FString Saves = BF6Project::ExperiencesRoot();
	if (!Saves.IsEmpty())
	{
		TArray<FString> Owned;
		FM.FindFiles(Owned, *(Saves / TEXT("*")), false, true);
		for (const FString& E : Owned)
		{
			const FString Full = FPaths::Combine(Saves, E);
			if (FPaths::FileExists(FPaths::Combine(Full, TEXT("package.json"))))
			{
				Out.AddUnique(Full);
			}
		}
	}
}

// DOES THIS REQUEST MEAN THE PROJECT THAT IS ACTUALLY OPEN?
//
// A rel path is only meaningful against a project, and the page and this side
// can disagree about which project that is without either noticing. The way
// they come apart is an open this side CARRIED OUT whose acknowledgment never
// reached the page: the page times it out, believes it is still in the old
// project, and its next save arrives here as a bare rel path that this side
// resolves against the new one. That is one project's text written into
// another, reported as a successful save, with the screen still naming the
// project it came from.
//
// So a request that means "in the project I have open" says which project it
// means, and this refuses it when the answer is no longer that project. The
// generation is checked as well as the path, because closing and reopening the
// same directory is still a different state of the world.
//
// A request that WRITES must say. Not saying is refused rather than allowed
// through, because that is the case that loses somebody's work, and a page old
// enough not to stamp its writes is a page that must not be writing. The
// reading requests are checked when they say and let through when they do not,
// so a stale read is a wrong answer on screen rather than a panel that cannot
// start.
static bool RequestNamesTheOpenProject(const FString& Op, const TSharedRef<FJsonObject>& In, FString& OutWhy)
{
	// The closed list. open, new, projects and status are how a project gets
	// CHOSEN, so they cannot be about one that is already open.
	static const TCHAR* kScoped[] = {
		TEXT("read"), TEXT("write"), TEXT("newfile"), TEXT("files"), TEXT("types"),
		TEXT("pending"), TEXT("install"), TEXT("build"), TEXT("run"),
		TEXT("push"), TEXT("pull"), TEXT("sitesave")
	};
	bool bScoped = false;
	for (const TCHAR* K : kScoped) { if (Op == K) { bScoped = true; break; } }
	if (!bScoped) return true;

	FString Named;
	double  Gen = -1.0;
	const bool bSaid = In->TryGetStringField(TEXT("proj"), Named) && !Named.IsEmpty()
		&& In->TryGetNumberField(TEXT("projGen"), Gen);

	const bool bWrites = (Op == TEXT("write") || Op == TEXT("newfile"));
	if (!bSaid)
	{
		if (!bWrites) return true;
		OutWhy = TEXT("That save did not say which project it was for, so nothing was written. ")
			TEXT("Close and reopen the Script tab, then save again.");
		return false;
	}

	const bool bSamePath = FPaths::ConvertRelativePathToFull(Named)
		.Equals(FPaths::ConvertRelativePathToFull(GProjectDir), ESearchCase::IgnoreCase);
	if (bSamePath && (uint32)Gen == GProjectGen) return true;

	UE_LOG(LogBF6Script, Warning,
		TEXT("refused %s: it was for %s (generation %d), and %s (generation %u) is open."),
		*Op, *Named, (int32)Gen,
		GProjectDir.IsEmpty() ? TEXT("no project") : *GProjectDir, GProjectGen);
	OutWhy = FString::Printf(
		TEXT("That request was for %s, and the tool has %s open, so nothing was done. ")
		TEXT("The editor and the tool had drifted apart. Close and reopen the Script tab to get them back in step."),
		*FPaths::GetCleanFilename(Named),
		GProjectDir.IsEmpty() ? TEXT("no project") : *FPaths::GetCleanFilename(GProjectDir));
	return false;
}

// ---------------------------------------------------------------------------
// THE REQUEST TABLE.
//
// One function, because every op is small and the shape of the whole
// conversation is easier to hold in your head in one place than spread over
// thirty handlers.
// ---------------------------------------------------------------------------
static void HandleEditorCall(int32 Id, const FString& Op, const TSharedRef<FJsonObject>& In)
{
	IFileManager& FM = IFileManager::Get();

	// Before anything else, and for every op at once, so a new op cannot be
	// added below without this applying to it.
	{
		FString Why;
		if (!RequestNamesTheOpenProject(Op, In, Why)) { ReplyFail(Id, Why); return; }
	}

	// ---- what the tool knows ------------------------------------------------
	if (Op == TEXT("status"))
	{
		SendStatus(Id);
		return;
	}

	// One panel state, remembered. Nothing else may be written through here:
	// the key list is closed, so a page that sends something unexpected gets a
	// refusal rather than a new line in the editor ini.
	if (Op == TEXT("pref"))
	{
		FString Name, Value;
		In->TryGetStringField(TEXT("name"), Name);
		In->TryGetStringField(TEXT("value"), Value);
		if (!IsKnownScriptPref(Name))
		{
			ReplyFail(Id, FString::Printf(TEXT("%s is not a panel state this editor keeps."), *Name));
			return;
		}
		if (GConfig)
		{
			GConfig->SetString(kPrefSection, *ScriptPrefIniKey(Name), *Value, GEditorPerProjectIni);
			GConfig->Flush(false, GEditorPerProjectIni);
		}
		ReplyOk(Id);
		return;
	}

	if (Op == TEXT("workersrc"))
	{
		const FString Path = EnsureWorkerSourceFile();
		TSharedRef<FJsonObject> O = MakeShared<FJsonObject>();
		O->SetBoolField(TEXT("ok"), !Path.IsEmpty());
		O->SetStringField(TEXT("url"), Path.IsEmpty() ? FString() : FileUrl(Path));
		if (Path.IsEmpty()) O->SetStringField(TEXT("why"), TEXT("Monaco's worker files are not vendored, so live error checking is off."));
		Reply(Id, O);
		return;
	}

	if (Op == TEXT("snippets"))
	{
		PushPayload(TEXT("snippets"), TEXT(""), ReadSnippetPacks());
		ReplyOk(Id);
		return;
	}

	// ---- the parsed API -----------------------------------------------------
	//
	// api.json is the SDK's own typings run through the TypeScript compiler:
	// every command with its real parameter names, types, optionality, doc
	// comment and overloads, plus the events and the enum member tables. It is
	// what the editor's completion, signature help and inlay hints are built
	// from, in place of a regex over the .d.ts text that cannot see any of
	// that. It ships with the plugin rather than being read out of the user's
	// node_modules, so it is there before a project has ever been installed.
	//
	// Same delivery as the snippets: a file:// page cannot fetch, so it goes
	// over the bridge as one chunked payload. A build without the file is not
	// broken, it is only poorer, so the page is told plainly and keeps using
	// its own symbol index.
	if (Op == TEXT("api"))
	{
		const FString Path = FPaths::Combine(ScriptRes(), TEXT("api.json"));
		FString Text;
		if (!FFileHelper::LoadFileToString(Text, *Path))
		{
			ReplyFail(Id, TEXT("api.json is not in this build's script resources, so completion falls back to the symbol index."));
			return;
		}
		PushPayload(TEXT("api"), TEXT(""), Text);
		ReplyOk(Id);
		return;
	}

	if (Op == TEXT("faq"))
	{
		const FString Path = FPaths::Combine(ScriptRes(), TEXT("faq.json"));
		FString Text;
		if (!FFileHelper::LoadFileToString(Text, *Path))
		{
			ReplyFail(Id, TEXT("The community answers file is missing. It is built by scratchpad build_faq.py from the Portal SDK research corpus."));
			return;
		}
		PushPayload(TEXT("faq"), TEXT(""), Text);
		ReplyOk(Id);
		return;
	}

	// ---- the curated answers ------------------------------------------------
	//
	// Three themed sets, pushed as three payloads so the page can keep them
	// apart from the mined faq.json. A missing set is not an error: a tool
	// built before the answer files existed still works, it just has less to
	// offer, and saying "the file is missing" to a creator who never asked for
	// it would be noise.
	if (Op == TEXT("answers"))
	{
		static const TCHAR* kThemes[] = { TEXT("players"), TEXT("systems"), TEXT("presentation") };
		int32 Loaded = 0;
		for (const TCHAR* Theme : kThemes)
		{
			const FString Path = FPaths::Combine(AnswersRes(), FString(Theme) + TEXT(".json"));
			FString Text;
			if (!FFileHelper::LoadFileToString(Text, *Path))
			{
				UE_LOG(LogBF6Script, Display, TEXT("No curated answers for %s at %s."), Theme, *Path);
				continue;
			}
			PushPayload(TEXT("answers"), Theme, Text);
			++Loaded;
		}
		TSharedRef<FJsonObject> O = MakeShared<FJsonObject>();
		O->SetBoolField(TEXT("ok"), true);
		O->SetNumberField(TEXT("sets"), Loaded);
		Reply(Id, O);
		return;
	}

	// A curated answer's block example, handed to the blocks editor.
	//
	// The same seam the UI builder uses: write the snippet where the blocks
	// editor already looks, open its tab, ask it to load that name. Nothing in
	// BF6Blocks changes.
	// SHOW A FILE FROM THIS EDITOR AS BLOCKS.
	//
	// The sibling op below hands over a snippet that an FAQ answer already
	// carries as block JSON. This one is for real source, which nothing here
	// can convert: the TypeScript-to-blocks converter lives only on the blocks
	// page. So the source travels there and that page converts and pastes it.
	//
	// The whole file goes, not the one function, because a function stripped of
	// its imports, module variables and event registration converts into far
	// less than it means. The blocks page centres the requested name.
	if (Op == TEXT("toblocksSource"))
	{
		FString Source, Wanted, File;
		In->TryGetStringField(TEXT("source"), Source);
		In->TryGetStringField(TEXT("name"), Wanted);
		In->TryGetStringField(TEXT("file"), File);
		if (Source.IsEmpty()) { ReplyFail(Id, TEXT("There is no source in that file to show.")); return; }

		BF6Blocks::Open();
		TSharedRef<FJsonObject> Ask = MakeShared<FJsonObject>();
		Ask->SetStringField(TEXT("op"), TEXT("sourceToBlocks"));
		Ask->SetStringField(TEXT("source"), Source);
		Ask->SetStringField(TEXT("name"), Wanted);
		Ask->SetStringField(TEXT("file"), File.IsEmpty() ? TEXT("source.ts") : File);
		BF6Blocks::HandleMessage(JsonOf(Ask));

		UE_LOG(LogBF6Script, Display, TEXT("Sent %s (%d chars) to the blocks editor to show as blocks."),
			Wanted.IsEmpty() ? TEXT("the open file") : *Wanted, Source.Len());
		ReplyOk(Id, TEXT("The block editor has it."));
		return;
	}

	if (Op == TEXT("toblocks"))
	{
		FString AnswerId, Snippet;
		In->TryGetStringField(TEXT("id"), AnswerId);
		In->TryGetStringField(TEXT("snippet"), Snippet);
		if (Snippet.IsEmpty()) { ReplyFail(Id, TEXT("That answer carries no block example.")); return; }

		// A name that cannot climb out of the snippet folder.
		FString Safe;
		for (const TCHAR C : AnswerId)
			if (FChar::IsAlnum(C) || C == TEXT('-') || C == TEXT('_')) Safe.AppendChar(C);
		if (Safe.IsEmpty()) Safe = TEXT("answer");

		const FString File = Safe + TEXT(".json");
		FString Why;
		if (!WriteFileAtomic(FPaths::Combine(BlocksSnippetDir(), File), Snippet, Why))
		{
			ReplyFail(Id, Why);
			return;
		}

		BF6Blocks::Open();
		TSharedRef<FJsonObject> Ask = MakeShared<FJsonObject>();
		Ask->SetStringField(TEXT("op"), TEXT("snippetLoad"));
		Ask->SetStringField(TEXT("name"), TEXT("faq/") + File);
		BF6Blocks::HandleMessage(JsonOf(Ask));

		UE_LOG(LogBF6Script, Display, TEXT("Handed %s to the blocks editor as snippet faq/%s."), *AnswerId, *File);
		ReplyOk(Id, TEXT("The block editor has it. Look for the rule it just loaded."));
		return;
	}

	if (Op == TEXT("openurl"))
	{
		FString Url;
		In->TryGetStringField(TEXT("url"), Url);
		if (Url.StartsWith(TEXT("http://")) || Url.StartsWith(TEXT("https://")))
		{
			FPlatformProcess::LaunchURL(*Url, nullptr, nullptr);
			ReplyOk(Id);
		}
		else
		{
			ReplyFail(Id, TEXT("Only a web address can be opened this way."));
		}
		return;
	}

	// ---- projects -----------------------------------------------------------
	if (Op == TEXT("projects"))
	{
		TArray<FString> Dirs;
		ListProjects(Dirs);
		TArray<TSharedPtr<FJsonValue>> List;
		for (const FString& D : Dirs)
		{
			TSharedRef<FJsonObject> P = MakeShared<FJsonObject>();
			FString Name = FPaths::GetCleanFilename(D);
			FString Exp;
			if (Name == TEXT("script")) { Exp = FPaths::GetCleanFilename(FPaths::GetPath(D)); Name = Exp; }
			P->SetStringField(TEXT("name"), Name);
			P->SetStringField(TEXT("path"), D);
			P->SetStringField(TEXT("experience"), Exp);
			List.Add(MakeShared<FJsonValueObject>(P));
		}
		TSharedRef<FJsonObject> O = MakeShared<FJsonObject>();
		O->SetBoolField(TEXT("ok"), true);
		O->SetArrayField(TEXT("list"), List);
		// WHICH ONE IS OPEN IS THE HOST'S ANSWER.
		//
		// The page opened list[0] at boot when nothing told it otherwise, so an
		// experience the tool had just imported and opened lost to whichever
		// project happened to sort first - a leftover example project, in the
		// case that made this obvious. The page already defers to the host once
		// a switch is under way; this is the same rule at boot.
		O->SetStringField(TEXT("open"), GProjectDir);
		Reply(Id, O);
		return;
	}

	if (Op == TEXT("open"))
	{
		FString Path;
		In->TryGetStringField(TEXT("path"), Path);
		if (!FPaths::FileExists(FPaths::Combine(Path, TEXT("package.json"))))
		{
			ReplyFail(Id, FString::Printf(TEXT("There is no script project at %s."), *Path));
			return;
		}
		OpenProject(Path);
		SendStatus(Id);
		return;
	}

	if (Op == TEXT("new"))
	{
		FString Name, Desc, Boiler, SeedFile, SeedText, SeedStrings;
		In->TryGetStringField(TEXT("name"), Name);
		In->TryGetStringField(TEXT("description"), Desc);
		In->TryGetStringField(TEXT("boilerplate"), Boiler);
		In->TryGetStringField(TEXT("seedFile"), SeedFile);
		In->TryGetStringField(TEXT("seedText"), SeedText);
		In->TryGetStringField(TEXT("seedStrings"), SeedStrings);
		if (Name.IsEmpty()) { ReplyFail(Id, TEXT("A project needs a name.")); return; }

		const FString Dir = FPaths::Combine(ScriptSaved(), KebabCase(Name));
		if (FPaths::DirectoryExists(Dir))
		{
			ReplyFail(Id, FString::Printf(TEXT("There is already a project at %s. Pick another name, or open that one."), *Dir));
			return;
		}

		FString Why;
		if (!CopyTemplate(Dir, Why)) { ReplyFail(Id, Why); return; }
		// NO EXPERIENCE ID. A project made here lives in the unlinked folder, so
		// it belongs to no experience yet. This used to pass GExperienceId, which
		// stamped the PREVIOUSLY open project's experience into the new project's
		// .env, and from there into every destination check the new project made.
		// Linking is a deliberate act: BF6.Script.LinkToPage.
		if (!ApplyInit(Dir, Name, Desc, Boiler.IsEmpty() ? TEXT("minimal") : Boiler, FString(), Why))
		{
			ReplyFail(Id, Why);
			return;
		}

		// A recipe brings its own first file with it.
		if (!SeedFile.IsEmpty() && !SeedText.IsEmpty())
		{
			FFileHelper::SaveStringToFile(SeedText, *FPaths::Combine(Dir, SeedFile));
		}
		if (!SeedStrings.IsEmpty() && SeedStrings != TEXT("{}"))
		{
			FFileHelper::SaveStringToFile(SeedStrings, *FPaths::Combine(Dir, TEXT("src"), TEXT("strings.json")));
		}

		OpenProject(Dir);

		TSharedRef<FJsonObject> O = MakeShared<FJsonObject>();
		O->SetBoolField(TEXT("ok"), true);
		O->SetStringField(TEXT("path"), Dir);
		Reply(Id, O);

		// Straight into npm install: the project is not usable without it, and
		// a beginner has no reason to know that.
		if (!GNodeExe.IsEmpty() && !GNpmCli.IsEmpty())
		{
			FString W2;
			Progress(TEXT("Installing what the project needs. The first time takes a minute."), TEXT("d"));
			StartRun(GNodeExe, FString::Printf(TEXT("\"%s\" install --no-audit --no-fund"), *GNpmCli),
				Dir, TEXT("install"), 0, W2, FString(), FString());
		}
		else
		{
			Progress(TEXT("Node is not installed, so the project's dependencies were not fetched. Install Node from nodejs.org, then press INSTALL."), TEXT("w"));
		}
		return;
	}

	// ---- files --------------------------------------------------------------
	// THE PAGE'S OWN VOICE IN THE LOG.
	//
	// Everything the panel does happens inside a browser whose console goes
	// nowhere, so working out why it opened the wrong file meant reasoning about
	// code instead of reading what it did. One op fixes that: the page can say
	// what it decided, and the answer lands in the same log as everything else.
	if (Op == TEXT("note"))
	{
		FString Text;
		In->TryGetStringField(TEXT("text"), Text);
		if (!Text.IsEmpty())
		{
			UE_LOG(LogBF6Script, Display, TEXT("page: %s"), *Text);
		}
		ReplyOk(Id);
		return;
	}

	// ---- the user's own AI --------------------------------------------------
	//
	// BF6Assist has done this work since it was written and nothing has ever
	// called it from a page: the seam existed, the console commands worked, and
	// no editor could reach it. These two ops are that route.
	//
	// The key never comes back this way. The page sends a briefing and a
	// question and receives text; the credential is read from the environment
	// inside Ask and exists only for the length of one request. The page is a
	// file:// document, so a key that reached it would effectively be published.
	if (Op == TEXT("assistStatus"))
	{
		const BF6Assist::FProvider P = BF6Assist::CurrentProvider();
		TSharedRef<FJsonObject> O = MakeShared<FJsonObject>();
		O->SetBoolField(TEXT("ok"), true);
		O->SetBoolField(TEXT("linked"), !P.Endpoint.IsEmpty());
		O->SetBoolField(TEXT("local"), P.bIsLocal);
		O->SetBoolField(TEXT("hasKey"), P.bHasKey);
		O->SetStringField(TEXT("model"), P.Model);
		// The setup steps come from the host so the page cannot drift out of
		// step with what the commands are actually called. No key, no endpoint
		// and no path go with it - only the instructions.
		O->SetStringField(TEXT("help"), BF6Assist::SetupHelp());
		Reply(Id, O);
		return;
	}

	if (Op == TEXT("assistAsk"))
	{
		FString Briefing, Question;
		In->TryGetStringField(TEXT("briefing"), Briefing);
		In->TryGetStringField(TEXT("question"), Question);
		if (Question.TrimStartAndEnd().IsEmpty())
		{
			ReplyFail(Id, TEXT("there was no question to ask"));
			return;
		}
		// Answered whenever the request comes back, which may be many seconds.
		// The page holds the reply against its own id, so a slow answer lands
		// in the right conversation even if the user has asked something else
		// in the meantime.
		BF6Assist::Ask(Briefing, Question, [Id](bool bOk, const FString& Text)
		{
			if (!bOk) { ReplyFail(Id, Text); return; }
			TSharedRef<FJsonObject> O = MakeShared<FJsonObject>();
			O->SetBoolField(TEXT("ok"), true);
			O->SetStringField(TEXT("text"), Text);
			Reply(Id, O);
		});
		return;
	}

	// ---- sounds and effects -------------------------------------------------
	if (Op == TEXT("sfxlist"))
	{
		TArray<TSharedPtr<FJsonValue>> Names;
		GatherSpawnNames(GProjectDir, Names);

		// The soundboard's own notes about each clip, when there is one. These
		// are not decoration: two of these sounds CRASH THE GAME, and several
		// are silent. Offering them without saying so would be handing somebody
		// a loaded foot-gun in a search box.
		TMap<FString, TSharedPtr<FJsonObject>> Notes;
		const FString SB = SoundboardDir();
		if (!SB.IsEmpty())
		{
			FString Text;
			if (FFileHelper::LoadFileToString(Text, *FPaths::Combine(SB, TEXT("manifest.json"))))
			{
				TArray<TSharedPtr<FJsonValue>> Arr;
				const TSharedRef<TJsonReader<>> R = TJsonReaderFactory<>::Create(Text);
				if (FJsonSerializer::Deserialize(R, Arr))
				{
					for (const TSharedPtr<FJsonValue>& V : Arr)
					{
						const TSharedPtr<FJsonObject>* O = nullptr;
						if (!V.IsValid() || !V->TryGetObject(O) || !O) continue;
						FString N;
						if ((*O)->TryGetStringField(TEXT("name"), N) && !N.IsEmpty()) Notes.Add(N, *O);
					}
				}
			}
		}
		for (TSharedPtr<FJsonValue>& V : Names)
		{
			const TSharedPtr<FJsonObject> O = V->AsObject();
			if (!O.IsValid()) continue;
			FString N = O->GetStringField(TEXT("name"));
			if (const TSharedPtr<FJsonObject>* Note = Notes.Find(N))
			{
				bool b = false;
				if ((*Note)->TryGetBoolField(TEXT("crash"), b) && b)      O->SetBoolField(TEXT("crash"), true);
				if ((*Note)->TryGetBoolField(TEXT("silent"), b) && b)     O->SetBoolField(TEXT("silent"), true);
				if ((*Note)->TryGetBoolField(TEXT("unreliable"), b) && b) O->SetBoolField(TEXT("unreliable"), true);
				double D = 0.0;
				if ((*Note)->TryGetNumberField(TEXT("dur"), D)) O->SetNumberField(TEXT("dur"), D);
				O->SetBoolField(TEXT("hasClip"), true);
			}
		}

		TSharedRef<FJsonObject> Out = MakeShared<FJsonObject>();
		Out->SetBoolField(TEXT("ok"), true);
		Out->SetArrayField(TEXT("items"), Names);
		Out->SetStringField(TEXT("preview"), PreviewSource());
		Reply(Id, Out);
		return;
	}

	// PLAY IT FROM THE GAME, when the add-on can. This decodes the real asset
	// at its real quality rather than a recording of it, and it plays through
	// the editor's own audio rather than through the page - so nothing has to
	// cross the browser bridge at all.
	if (Op == TEXT("sfxplay"))
	{
		FString Name;
		In->TryGetStringField(TEXT("name"), Name);
		if (Name.IsEmpty()) { ReplyFail(Id, TEXT("No sound named.")); return; }
		if (!BF6UiSound::CanPreviewPlaceable(Name))
		{
			ReplyFail(Id, FString::Printf(
				TEXT("%s cannot be played from the install here."), *Name));
			return;
		}
		BF6UiSound::TogglePreviewPlaceable(Name);
		TSharedRef<FJsonObject> O = MakeShared<FJsonObject>();
		O->SetBoolField(TEXT("ok"), true);
		O->SetStringField(TEXT("playing"), BF6UiSound::PreviewPlaying());
		Reply(Id, O);
		return;
	}

	if (Op == TEXT("sfxstop"))
	{
		BF6UiSound::StopPreview();
		ReplyOk(Id);
		return;
	}

	if (Op == TEXT("sfxclip"))
	{
		FString Name;
		In->TryGetStringField(TEXT("name"), Name);
		const FString SB = SoundboardDir();
		if (SB.IsEmpty() || Name.IsEmpty())
		{
			ReplyFail(Id, TEXT("There is no sound library configured to play from."));
			return;
		}
		// The manifest says which file a name maps to; nothing is guessed from
		// the name, because the folder layout is the recorder's business.
		FString Text, Rel;
		if (FFileHelper::LoadFileToString(Text, *FPaths::Combine(SB, TEXT("manifest.json"))))
		{
			TArray<TSharedPtr<FJsonValue>> Arr;
			const TSharedRef<TJsonReader<>> R = TJsonReaderFactory<>::Create(Text);
			if (FJsonSerializer::Deserialize(R, Arr))
			{
				for (const TSharedPtr<FJsonValue>& V : Arr)
				{
					const TSharedPtr<FJsonObject>* O = nullptr;
					if (!V.IsValid() || !V->TryGetObject(O) || !O) continue;
					FString N;
					if ((*O)->TryGetStringField(TEXT("name"), N) && N == Name)
					{
						(*O)->TryGetStringField(TEXT("file"), Rel);
						break;
					}
				}
			}
		}
		if (Rel.IsEmpty()) { ReplyFail(Id, FString::Printf(TEXT("No clip for %s."), *Name)); return; }

		TArray<uint8> Bytes;
		if (!FFileHelper::LoadFileToArray(Bytes, *FPaths::Combine(SB, Rel)))
		{
			ReplyFail(Id, FString::Printf(TEXT("Could not read the clip for %s."), *Name));
			return;
		}
		TSharedRef<FJsonObject> Out = MakeShared<FJsonObject>();
		Out->SetBoolField(TEXT("ok"), true);
		Out->SetStringField(TEXT("name"), Name);
		// Base64 rather than a file url: the page is a file:// document and is
		// not allowed to reach outside its own folder.
		Out->SetStringField(TEXT("audio"), FBase64::Encode(Bytes));
		Reply(Id, Out);
		return;
	}

	if (Op == TEXT("files"))
	{
		TArray<FString> Rel;
		GatherProjectFiles(Rel);
		TArray<TSharedPtr<FJsonValue>> List;
		for (const FString& R : Rel) List.Add(MakeShared<FJsonValueString>(R));
		TSharedRef<FJsonObject> O = MakeShared<FJsonObject>();
		O->SetBoolField(TEXT("ok"), true);
		O->SetArrayField(TEXT("files"), List);
		// Handed over once and then forgotten: the page's own choice is right
		// every other time, and an override that stuck would fight the user
		// every time they opened a different file.
		if (!GPreferredOpen.IsEmpty() && Rel.Contains(GPreferredOpen))
		{
			UE_LOG(LogBF6Script, Display,
				TEXT("Script: telling the page to show %s first."), *GPreferredOpen);
			O->SetStringField(TEXT("showFirst"), GPreferredOpen);
			GPreferredOpen.Reset();
		}
		else if (!GPreferredOpen.IsEmpty())
		{
			// Asked for a file this project does not list. Said out loud,
			// because the alternative is the panel quietly opening the wrong
			// thing and nobody knowing why.
			UE_LOG(LogBF6Script, Warning,
				TEXT("Script: was asked to show %s first, but it is not in this project's %d file(s)."),
				*GPreferredOpen, Rel.Num());
			GPreferredOpen.Reset();
		}
		Reply(Id, O);
		return;
	}

	if (Op == TEXT("read"))
	{
		FString Rel;
		In->TryGetStringField(TEXT("rel"), Rel);
		if (GProjectDir.IsEmpty() || Rel.Contains(TEXT(".."))) { ReplyFail(Id, TEXT("No project, or a path that leaves it.")); return; }
		FString Text;
		if (!FFileHelper::LoadFileToString(Text, *FPaths::Combine(GProjectDir, Rel)))
		{
			ReplyFail(Id, FString::Printf(TEXT("Could not read %s."), *Rel));
			return;
		}
		PushPayload(TEXT("file"), Rel, Text);
		ReplyOk(Id);
		return;
	}

	if (Op == TEXT("write"))
	{
		FString Rel, Text;
		In->TryGetStringField(TEXT("rel"), Rel);
		In->TryGetStringField(TEXT("text"), Text);
		if (GProjectDir.IsEmpty() || Rel.Contains(TEXT(".."))) { ReplyFail(Id, TEXT("No project, or a path that leaves it.")); return; }
		const FString Full = FPaths::Combine(GProjectDir, Rel);

		// Unchanged is not written. The editor autosaves on a timer and a
		// no-op save would otherwise fill .history with identical copies.
		FString Was;
		if (FFileHelper::LoadFileToString(Was, *Full) && Was == Text) { ReplyOk(Id); return; }

		KeepHistory(Full, Rel);
		FString Why;
		if (!WriteFileAtomic(Full, Text, Why)) { ReplyFail(Id, Why); return; }
		ReplyOk(Id);
		return;
	}

	// ---- a new file, which must not already exist ---------------------------
	//
	// NEW FILE used to be an ordinary write, and the page decided whether a name
	// was free by trying to READ it first: any read error was taken as
	// permission to write. A locked file, or a permissions problem, therefore
	// looked exactly like an empty slot, and typing index.ts replaced the
	// project's entry point.
	//
	// The page now settles that from the project's own file listing, which is a
	// positive statement rather than the absence of an error. This closes the
	// gap that remains: between the page asking and the write landing, only the
	// host can be sure. So existence is decided here, next to the write, and a
	// name that is taken comes back as its own kind of refusal rather than a
	// generic failure the page has to interpret.
	if (Op == TEXT("newfile"))
	{
		FString Rel, Text;
		In->TryGetStringField(TEXT("rel"), Rel);
		In->TryGetStringField(TEXT("text"), Text);
		if (GProjectDir.IsEmpty() || Rel.IsEmpty() || Rel.Contains(TEXT("..")))
		{
			ReplyFail(Id, TEXT("No project is open, or that name would leave it."));
			return;
		}
		const FString Full = FPaths::Combine(GProjectDir, Rel);
		// Checked against the RESOLVED path, so a spelling the test above missed
		// still cannot land outside the project.
		if (!FPaths::IsUnderDirectory(FPaths::ConvertRelativePathToFull(Full),
			FPaths::ConvertRelativePathToFull(GProjectDir)))
		{
			ReplyFail(Id, TEXT("That name would put the file outside the project."));
			return;
		}
		if (FPaths::FileExists(Full))
		{
			// Its own reason code, because the page opens the existing file
			// rather than reporting an error the user can do nothing with.
			TSharedRef<FJsonObject> O = MakeShared<FJsonObject>();
			O->SetBoolField(TEXT("ok"), false);
			O->SetStringField(TEXT("reason"), TEXT("exists"));
			O->SetStringField(TEXT("why"), FString::Printf(
				TEXT("%s already exists in this project, and was not touched."), *Rel));
			Reply(Id, O);
			return;
		}
		IFileManager::Get().MakeDirectory(*FPaths::GetPath(Full), true);
		FString Why;
		if (!WriteFileAtomic(Full, Text, Why)) { ReplyFail(Id, Why); return; }
		ReplyOk(Id);
		return;
	}

	// ---- the pending bundle -------------------------------------------------
	if (Op == TEXT("pending"))
	{
		SendPending();
		ReplyOk(Id);
		return;
	}

	// ---- types --------------------------------------------------------------
	if (Op == TEXT("types"))
	{
		const int32 N = QueueTypeLibs();
		if (N == 0)
		{
			ReplyFail(Id, TEXT("No type definitions were found in this project's node_modules. Press INSTALL first."));
			return;
		}
		TSharedRef<FJsonObject> O = MakeShared<FJsonObject>();
		O->SetBoolField(TEXT("ok"), true);
		O->SetNumberField(TEXT("count"), N);
		// Which of the three sets were found, and what each one contributed.
		O->SetArrayField(TEXT("sources"), GTypeSources);
		Reply(Id, O);
		return;
	}

	// ---- node work ----------------------------------------------------------
	if (Op == TEXT("install") || Op == TEXT("build"))
	{
		if (GProjectDir.IsEmpty()) { ReplyFail(Id, TEXT("Open or create a project first.")); return; }
		if (GNodeExe.IsEmpty())
		{
			ReplyFail(Id, TEXT("Node is not installed, or not where the tool looks. Install it from nodejs.org (version 24 or newer), then reopen this tab. Everything else in the panel works without it."));
			return;
		}
		if (GNpmCli.IsEmpty())
		{
			ReplyFail(Id, TEXT("npm was not found beside node. Reinstall Node from nodejs.org so npm comes with it."));
			return;
		}
		FString Why;
		// BUILD is the checked build, never the bundler on its own.
		if (Op == TEXT("build"))
		{
			if (!StartCheckedBuild(Id, Why)) ReplyFail(Id, Why);
			return;
		}
		const FString Args = FString::Printf(TEXT("\"%s\" install --no-audit --no-fund"), *GNpmCli);
		if (!StartRun(GNodeExe, Args, GProjectDir, Op, Id, Why, FString(), FString())) ReplyFail(Id, Why);
		return;
	}

	// ---- the rest of the template's own tools -------------------------------
	//
	// Mike De Luca's template ships eight npm scripts and the tool only ever
	// ran two of them. The other six sat on disk with nothing offering them, so
	// a creator who did not read package.json never learned they existed.
	//
	// The list is CLOSED, and deploy is deliberately not on it: deploy signs in
	// with an EA session id, and this tool does not hold one and must not start.
	// PUT ON PORTAL is the path that ships a script from here.
	if (Op == TEXT("run"))
	{
		static const TCHAR* kRunnable[] = {
			TEXT("build"), TEXT("lint"), TEXT("prettier"), TEXT("refresh-ai"),
			TEXT("update"), TEXT("export-thumbnail"), TEXT("minify-spatials")
		};

		FString Script;
		In->TryGetStringField(TEXT("script"), Script);

		if (Script.StartsWith(TEXT("deploy")))
		{
			ReplyFail(Id, TEXT("Deploy is not offered here. It signs in to EA with a session id this tool does not hold. ")
				TEXT("Use PUT ON PORTAL, which pushes your built bundle into the page you already have open."));
			return;
		}

		bool bKnown = false;
		for (const TCHAR* K : kRunnable) if (Script == K) { bKnown = true; break; }
		if (!bKnown)
		{
			ReplyFail(Id, FString::Printf(TEXT("%s is not one of the template tools this editor runs."), *Script));
			return;
		}

		if (GProjectDir.IsEmpty()) { ReplyFail(Id, TEXT("Open or create a project first.")); return; }
		if (GNodeExe.IsEmpty())
		{
			ReplyFail(Id, TEXT("Node is not installed, or not where the tool looks. Install it from nodejs.org (version 24 or newer), then reopen this tab."));
			return;
		}
		if (GNpmCli.IsEmpty())
		{
			ReplyFail(Id, TEXT("npm was not found beside node. Reinstall Node from nodejs.org so npm comes with it."));
			return;
		}

		FString Why;
		// "build" from the template-tools list is the SAME build as the BUILD
		// button, type check included. Letting this one run the bundler alone
		// would be a second, unchecked way to produce a bundle that PUSH is
		// asked to send.
		if (Script == TEXT("build"))
		{
			if (!StartCheckedBuild(Id, Why)) ReplyFail(Id, Why);
			return;
		}
		if (!StartRun(GNodeExe, FString::Printf(TEXT("\"%s\" run %s"), *GNpmCli, *Script),
			GProjectDir, Script, Id, Why, FString(), FString()))
		{
			ReplyFail(Id, Why);
		}
		return;
	}

	// ---- the log tail -------------------------------------------------------
	if (Op == TEXT("logtail"))
	{
		bool bOn = false;
		In->TryGetBoolField(TEXT("on"), bOn);
		GTailing = bOn;
		GTailPath = PortalLogPath();
		if (bOn)
		{
			const int64 Size = FM.FileSize(*GTailPath);
			GTailOffset = FMath::Max<int64>(0, Size);   // from here on, not the whole history
			if (!GTailTick.IsValid())
			{
				GTailTick = FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateStatic(&TailTick), 0.5f);
			}
			TSharedRef<FJsonObject> O = MakeShared<FJsonObject>();
			O->SetBoolField(TEXT("ok"), true);
			O->SetStringField(TEXT("path"), GTailPath);
			O->SetBoolField(TEXT("missing"), Size < 0);
			Reply(Id, O);
		}
		else
		{
			ReplyOk(Id);
		}
		return;
	}

	// ---- the scene ----------------------------------------------------------
	if (Op == TEXT("objlabel"))
	{
		double Num = -1;
		In->TryGetNumberField(TEXT("objid"), Num);
		const int32 Want = (int32)Num;
		FString Label;
		const TArray<BF6Api::FObjIdRow> Rows = BF6Api::GatherObjIds();
		for (const BF6Api::FObjIdRow& R : Rows)
		{
			if (R.Id != Want) continue;
			Label = R.Name.IsEmpty() ? R.Type : (R.Name + TEXT("  (") + R.Type + TEXT(")"));
			break;
		}
		TSharedRef<FJsonObject> O = MakeShared<FJsonObject>();
		O->SetBoolField(TEXT("ok"), true);
		O->SetNumberField(TEXT("objid"), Want);
		O->SetStringField(TEXT("label"), Label);
		Reply(Id, O);
		return;
	}

	// ---- the site -----------------------------------------------------------
	if (Op == TEXT("opensite"))
	{
		FString Url = BF6PortalWeb::BaseUrl();
		if (!GExperienceId.IsEmpty())
		{
			// BaseUrl() ALREADY ENDS IN /bf6.
			//
			// Appending another one built .../bf6/bf6/experience/rules/script.
			// The tool's other URL builder, BF6PortalWeb::ExperienceUrl, gets
			// this right, so the two disagreed and only this one was wrong.
			Url = FString::Printf(TEXT("%s/experience/rules/script?id=%s"), *BF6PortalWeb::BaseUrl(), *GExperienceId);
		}
		BF6PortalWeb::Open(Url);
		ReplyOk(Id, TEXT("The Portal panel is opening. Sign in there if it asks; the tool never sees your password."));
		return;
	}

	if (Op == TEXT("push") || Op == TEXT("pull") || Op == TEXT("sitesave"))
	{
		if (GProjectDir.IsEmpty())
		{
			ReplyFail(Id, TEXT("Open a script project first."));
			return;
		}
		if (!GSiteOnScriptPage)
		{
			ReplyFail(Id, TEXT("The Portal panel is not on the experience's Script page. Open it there first."));
			return;
		}
		if (GSiteReq != 0)
		{
			ReplyFail(Id, TEXT("The last request to the page has not answered yet."));
			return;
		}

		// ---- WHERE IS THIS GOING? ------------------------------------------
		//
		// Three separate facts, and all three have to agree before anything
		// leaves this function:
		//
		//   the PROJECT's destination   GExperienceId, from where the project
		//                               lives or from a deliberate link;
		//   the PAGE's destination      GSitePageExperience, what the browser
		//                               reports it is actually showing;
		//   what the PAGE half claims   the id it sends with the request, when
		//                               it sends one.
		//
		// The rule of this whole tool is that an existing experience is never
		// modified by accident, so a disagreement, or a destination nobody can
		// name, is refused. Guessing is not available: one of the two guesses
		// destroys somebody else's work and the tool cannot tell which.
		const bool bWrites = (Op != TEXT("pull"));

		// AN UNKNOWN DESTINATION IS NOT A DESTINATION. This used to pass: the
		// comparison below only ran when BOTH ids were non-empty, so a page the
		// tool could not identify was treated as the right one.
		if (GSitePageExperience.IsEmpty())
		{
			UE_LOG(LogBF6Script, Warning, TEXT("refused %s: the tool cannot tell which experience the Portal page is for."), *Op);
			ReplyFail(Id, TEXT("The tool cannot tell which experience the Portal page is for, so it will not send anything to it. ")
				TEXT("Open the experience's Script page from the Portal panel and try again."));
			return;
		}

		if (bWrites && GExperienceId.IsEmpty())
		{
			UE_LOG(LogBF6Script, Warning,
				TEXT("refused %s: the open project is not linked to an experience, the page is on %s."), *Op, *GSitePageExperience);
			ReplyFail(Id, FString::Printf(
				TEXT("This project is not linked to an experience, so the tool will not write it to the page that happens to be open. ")
				TEXT("The Portal panel is on %s. If this project belongs there, run BF6.Script.LinkToPage in the console to link it, then press this again."),
				*GSitePageExperience));
			return;
		}

		if (!GExperienceId.IsEmpty() && !GExperienceId.Equals(GSitePageExperience, ESearchCase::IgnoreCase))
		{
			UE_LOG(LogBF6Script, Warning,
				TEXT("refused %s: the project belongs to experience %s, the page is on %s."),
				*Op, *GExperienceId, *GSitePageExperience);
			ReplyFail(Id, FString::Printf(TEXT("This project belongs to experience %s, but the Portal panel is on %s. ")
				TEXT("Open this project's Script page, or open the project that belongs to the page you are on."),
				*GExperienceId, *GSitePageExperience));
			return;
		}

		// The page half may say where IT believes it is about to write. When it
		// does, it has to agree with what the browser reports, or the two halves
		// are looking at different documents and neither is trustworthy.
		FString Declared;
		In->TryGetStringField(TEXT("experience"), Declared);
		if (!Declared.IsEmpty() && !Declared.Equals(GSitePageExperience, ESearchCase::IgnoreCase))
		{
			UE_LOG(LogBF6Script, Warning,
				TEXT("refused %s: the editor asked for experience %s, the panel reports %s."), *Op, *Declared, *GSitePageExperience);
			ReplyFail(Id, FString::Printf(
				TEXT("The editor asked to send this to experience %s and the Portal panel is on %s. ")
				TEXT("Nothing was sent. Refresh the Portal panel and try again."), *Declared, *GSitePageExperience));
			return;
		}

		// ---- IS IT FIT TO SEND? --------------------------------------------
		// A bundle only leaves here when a checked build made it out of exactly
		// the files that are on disk now. See THE CHECKED BUILD.
		if (Op == TEXT("push"))
		{
			FString Why;
			if (!ReadyToSend(Why))
			{
				UE_LOG(LogBF6Script, Warning, TEXT("refused push: %s"), *Why);
				ReplyFail(Id, Why);
				return;
			}
		}

		GSiteReq = Id;
		GSiteOp = Op;
		// WHAT THIS REQUEST IS BOUND TO. The answer is checked against both
		// before it is allowed to change anything, so a reply that arrives after
		// the panel has moved on cannot be read as a verdict about the page it
		// moved to.
		GSiteReqGen = GSitePageGen;
		GSiteReqExperience = GSitePageExperience;

		if (Op == TEXT("sitesave"))
		{
			BF6PortalWeb::Exec(FString::Printf(TEXT("try{window.BF6ScriptSync.save(%d)}catch(e){}"), Id));
			return;
		}
		if (Op == TEXT("pull"))
		{
			BF6PortalWeb::Exec(FString::Printf(TEXT("try{window.BF6ScriptSync.pull(%d)}catch(e){}"), Id));
			return;
		}

		// PUSH sends what the bundler produced, never what is merely on screen.
		//
		// With publish mode on it sends the lean bundle the build made from
		// that output, which is the file the whole setting exists to produce.
		// It is only ever the file the build itself wrote: no lean bundle, no
		// push, because the build refuses above rather than falling back to the
		// full one behind the user's back.
		FString Bundle, Strings;
		const bool bLean = BF6Script::PublishMode(GProjectDir);
		const FString BundlePath = bLean
			? BF6Script::LeanBundlePath(GProjectDir)
			: BundlePathFor(GProjectDir);
		const FString StringsPath = FPaths::Combine(GProjectDir, TEXT("dist"), TEXT("bundle.strings.json"));
		if (!FFileHelper::LoadFileToString(Bundle, *BundlePath))
		{
			GSiteReq = 0;
			GSiteReqExperience.Reset();
			ReplyFail(Id, TEXT("There is no dist/bundle.ts yet. Press BUILD first."));
			return;
		}
		FFileHelper::LoadFileToString(Strings, *StringsPath);

		UE_LOG(LogBF6Script, Display, TEXT("push: sending %d characters to experience %s."),
			Bundle.Len(), *GSiteReqExperience);
		BF6PortalWeb::Exec(FString::Printf(TEXT("try{window.BF6ScriptSync.push(%d,%s,%s)}catch(e){}"),
			Id, *JsQuote(Bundle), *JsQuote(Strings)));
		return;
	}

	// LINK THIS PROJECT TO THE PAGE. The deliberate act that replaces the
	// silent adoption that used to happen behind the user's back.
	if (Op == TEXT("linktopage"))
	{
		FString Why;
		if (!LinkProjectToPage(Why)) { ReplyFail(Id, Why); return; }
		SendStatus(Id);
		return;
	}

	ReplyFail(Id, FString::Printf(TEXT("The tool does not know the request '%s'."), *Op));
}

// ---------------------------------------------------------------------------
// What the SITE half says.
// ---------------------------------------------------------------------------
static void HandleSiteCall(const FString& Op, const TSharedRef<FJsonObject>& In)
{
	if (Op == TEXT("sitestate"))
	{
		bool bOn = false;
		In->TryGetBoolField(TEXT("onScriptPage"), bOn);
		GSiteOnScriptPage = bOn;

		FString Url, Exp, ScriptTab, StringsTab, TabSel;
		In->TryGetStringField(TEXT("url"), Url);
		In->TryGetStringField(TEXT("experience"), Exp);
		In->TryGetStringField(TEXT("scriptTab"), ScriptTab);
		In->TryGetStringField(TEXT("stringsTab"), StringsTab);
		In->TryGetStringField(TEXT("tabSelector"), TabSel);
		bool bMonaco = false;
		In->TryGetBoolField(TEXT("hasMonaco"), bMonaco);

		// THE PAGE'S IDENTITY, AND WHEN IT LAST CHANGED.
		//
		// The generation moves on any of the three: a different experience, a
		// different url, or leaving the Script page. A request records the
		// generation it went out under, so an answer from a page that has since
		// been replaced can be told apart from an answer about the page that is
		// there now. The experience id alone cannot do that.
		{
			const bool bMoved = (bOn != GSiteOnScriptPage) || (Url != GSitePageUrl)
				|| ((bOn ? Exp : FString()) != GSitePageExperience);
			GSiteOnScriptPage = bOn;
			GSitePageExperience = bOn ? Exp : FString();
			GSitePageUrl = Url;
			if (bMoved) GSitePageGen++;
		}

		// THE PROJECT'S DESTINATION IS NOT ADOPTED FROM THE BROWSER.
		//
		// This line used to read:
		//   if (bOn && !Exp.IsEmpty() && GExperienceId.IsEmpty()) GExperienceId = Exp;
		// so an unlinked project silently became "linked" to whatever experience
		// the panel happened to be showing, and every later push treated that
		// accident as the project's identity. Linking is a deliberate act now:
		// BF6.Script.LinkToPage, or the editor's linktopage request.

		// THE FALLBACK SIGN-OUT SIGNAL. The profile module owns the account
		// state and its two notifications are the fact; until they are wired,
		// a panel that was on a Script page and is now on the login page is
		// the only evidence available, and it is better than none. Both paths
		// end in the same call, so wiring the real one later changes nothing
		// downstream.
		{
			static bool bWasOnScriptPage = false;
			const bool bLogin = Url.Contains(TEXT("/login")) || Url.Contains(TEXT("accounts.ea.com"));
			if (bWasOnScriptPage && bLogin) BF6Script::NotifySessionLost(TEXT("the panel bounced to the login page"));
			if (!bWasOnScriptPage && bOn) BF6Script::NotifySessionRestored();
			bWasOnScriptPage = bOn;
		}

		GSiteState = FString::Printf(
			TEXT("script page %s, editor %s, script tab '%s', strings tab '%s', tabs found by '%s'"),
			bOn ? TEXT("yes") : TEXT("no"), bMonaco ? TEXT("found") : TEXT("not found"),
			*ScriptTab, StringsTab.IsEmpty() ? TEXT("none") : *StringsTab, *TabSel);
		UE_LOG(LogBF6Script, Verbose, TEXT("Portal page: %s"), *GSiteState);

		TSharedRef<FJsonObject> O = MakeShared<FJsonObject>();
		O->SetStringField(TEXT("kind"), TEXT("sitestate"));
		O->SetBoolField(TEXT("onScriptPage"), bOn);
		O->SetBoolField(TEXT("hasMonaco"), bMonaco);
		O->SetStringField(TEXT("url"), Url);
		Event(O);
		return;
	}

	if (Op == TEXT("siteresult"))
	{
		double ReqNum = 0;
		In->TryGetNumberField(TEXT("reqId"), ReqNum);
		const int32 ReqId = (int32)ReqNum;
		if (ReqId == 0 || ReqId != GSiteReq) return;

		// AN ANSWER IS ONLY AN ANSWER ABOUT THE PAGE THE REQUEST WENT TO.
		//
		// Nothing tied a reply to a page before, so a push that was still in
		// flight when the panel navigated came back as a success and was written
		// into the project's pending state as "the bundle is in the Portal page"
		// - a page that no longer existed, or worse, a different experience's.
		// The request recorded both the page generation and the experience it
		// was sent to; a reply that does not match them is stale by definition.
		if (GSitePageGen != GSiteReqGen ||
			!GSitePageExperience.Equals(GSiteReqExperience, ESearchCase::IgnoreCase))
		{
			const int32 Was = GSiteReq;
			const FString WasFor = GSiteReqExperience;
			GSiteReq = 0;
			GSiteOp.Reset();
			GSiteReqExperience.Reset();
			UE_LOG(LogBF6Script, Warning,
				TEXT("dropped a page answer for request %d: it was sent to experience %s and the panel is on %s now."),
				Was, *WasFor, GSitePageExperience.IsEmpty() ? TEXT("no experience") : *GSitePageExperience);
			ReplyFail(Was, FString::Printf(
				TEXT("The Portal panel moved while that was in flight, so the tool stopped and changed nothing here. ")
				TEXT("It was sent to experience %s and the panel is on %s now. Check that page before trying again."),
				*WasFor, GSitePageExperience.IsEmpty() ? TEXT("a page with no experience") : *GSitePageExperience));
			return;
		}

		const FString Which = GSiteOp;
		GSiteReq = 0;
		GSiteOp.Reset();
		GSiteReqExperience.Reset();

		bool bOk = false;
		In->TryGetBoolField(TEXT("ok"), bOk);
		FString Why, How, Text, FileName, Content, StringsTab, StringsWhy;
		In->TryGetStringField(TEXT("why"), Why);
		In->TryGetStringField(TEXT("how"), How);
		In->TryGetStringField(TEXT("text"), Text);
		In->TryGetStringField(TEXT("fileName"), FileName);
		In->TryGetStringField(TEXT("content"), Content);
		In->TryGetStringField(TEXT("stringsTab"), StringsTab);
		In->TryGetStringField(TEXT("stringsWhy"), StringsWhy);
		bool bPartial = false, bStringsWritten = false;
		In->TryGetBoolField(TEXT("partial"), bPartial);
		In->TryGetBoolField(TEXT("stringsWritten"), bStringsWritten);

		if (!bOk) { ReplyFail(ReqId, Why); return; }

		if (Which == TEXT("pull"))
		{
			// A bundle goes to dist so it can be read; anything else is source
			// and goes where source belongs.
			const bool bBundled = Content.Contains(TEXT("bf6-portal-bundler"))
				|| Content.Contains(TEXT("// bundle"))
				|| Content.Len() > 60000;
			const FString Rel = bBundled ? TEXT("dist/bundle.ts") : TEXT("src/index.ts");
			if (!GProjectDir.IsEmpty())
			{
				const FString Full = FPaths::Combine(GProjectDir, Rel);
				IFileManager::Get().MakeDirectory(*FPaths::GetPath(Full), true);
				FFileHelper::SaveStringToFile(Content, *Full, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
			}
			TSharedRef<FJsonObject> O = MakeShared<FJsonObject>();
			O->SetBoolField(TEXT("ok"), true);
			O->SetStringField(TEXT("rel"), Rel);
			O->SetStringField(TEXT("content"), Content);
			O->SetStringField(TEXT("text"), FString::Printf(
				TEXT("Read %d characters from the page (%s) into %s.%s"),
				Content.Len(), *How, *Rel,
				bPartial ? TEXT(" The page's own editor object was not reachable, so this came from its text box and may be short.") : TEXT("")));
			Reply(ReqId, O);
			return;
		}

		if (Which == TEXT("push"))
		{
			TSharedRef<FJsonObject> O = MakeShared<FJsonObject>();
			O->SetBoolField(TEXT("ok"), true);
			O->SetBoolField(TEXT("stringsTab"), !StringsTab.IsEmpty());
			FString Msg = FString::Printf(TEXT("Bundle written into the page (%s). Press SAVE ON PORTAL, or press Save on the page yourself."), *How);
			if (!StringsTab.IsEmpty())
			{
				Msg += bStringsWritten
					? FString::Printf(TEXT(" Strings written into the '%s' tab."), *StringsTab)
					: FString::Printf(TEXT(" The '%s' tab would not take the strings: %s"), *StringsTab, *StringsWhy);
			}
			else if (!StringsWhy.IsEmpty())
			{
				Msg += TEXT(" ") + StringsWhy + TEXT(" Upload dist/bundle.strings.json through MANAGE SCRIPTS on the site.");
			}
			O->SetStringField(TEXT("text"), Msg);
			Reply(ReqId, O);
			WritePending(TEXT("pushed"), TEXT("Bundle is in the Portal page but not saved on Portal yet."));
			SendPending();
			return;
		}

		if (Which == TEXT("sitesave"))
		{
			// The press landed. Whether the SITE accepted it comes separately,
			// as a verdict, because the site answers on its own timing.
			WritePending(TEXT("pushed"), TEXT("Save pressed on Portal. Waiting for the site's answer."));
			SendPending();
		}
		ReplyOk(ReqId, Text.IsEmpty() ? TEXT("The page did the thing and said nothing.") : Text);
		return;
	}

	// ---- THE SITE'S VERDICT ON A SAVE --------------------------------------
	//
	// Three things arrive together, any of which may be empty:
	//   status   the gRPC status the site's own save call came back with, read
	//            out of the trailer frame, plus its message;
	//   markers  the site's Monaco markers on its model, which are the closest
	//            thing to a compiler telling us exactly where;
	//   toast    whatever the site put on screen, verbatim.
	//
	// Every marker is mapped through the line map back to the file the user
	// wrote, so the error lands where they can act on it. Anything that cannot
	// be mapped is handed over whole rather than pinned to a wrong line.
	if (Op == TEXT("siteverdict"))
	{
		// A VERDICT IS ABOUT ONE EXPERIENCE'S SAVE, AND IT LANDS IN THAT
		// EXPERIENCE'S PROJECT OR NOWHERE.
		//
		// This writes the project's pending state and maps Portal's error lines
		// through the open project's line map. Doing that for a save on some
		// other experience would stamp "saved on Portal" onto a project that was
		// never saved, and point its errors at unrelated source lines.
		if (!GExperienceId.IsEmpty() && !GSitePageExperience.IsEmpty() &&
			!GExperienceId.Equals(GSitePageExperience, ESearchCase::IgnoreCase))
		{
			UE_LOG(LogBF6Script, Warning,
				TEXT("ignored a Portal save verdict: it is about experience %s and the open project belongs to %s."),
				*GSitePageExperience, *GExperienceId);
			return;
		}

		double StatusNum = -1;
		In->TryGetNumberField(TEXT("grpcStatus"), StatusNum);
		FString GrpcMessage, Toast, Where;
		In->TryGetStringField(TEXT("grpcMessage"), GrpcMessage);
		In->TryGetStringField(TEXT("toast"), Toast);
		In->TryGetStringField(TEXT("where"), Where);

		const int32 Status = (int32)StatusNum;
		const TArray<TSharedPtr<FJsonValue>>* Markers = nullptr;
		In->TryGetArrayField(TEXT("markers"), Markers);

		TArray<TSharedPtr<FJsonValue>> Mapped, Unmapped;
		if (Markers)
		{
			for (const TSharedPtr<FJsonValue>& V : *Markers)
			{
				const TSharedPtr<FJsonObject>* M = nullptr;
				if (!V.IsValid() || !V->TryGetObject(M)) continue;

				double Ln = 0, Col = 1, Sev = 8;
				FString Msg;
				(*M)->TryGetNumberField(TEXT("line"), Ln);
				(*M)->TryGetNumberField(TEXT("column"), Col);
				(*M)->TryGetNumberField(TEXT("severity"), Sev);
				(*M)->TryGetStringField(TEXT("message"), Msg);

				FString Rel; int32 SrcLine = 0;
				TSharedRef<FJsonObject> Out = MakeShared<FJsonObject>();
				Out->SetStringField(TEXT("message"), Msg);
				Out->SetNumberField(TEXT("column"), Col);
				Out->SetNumberField(TEXT("severity"), Sev);
				Out->SetNumberField(TEXT("bundleLine"), Ln);
				if (MapBundleLine((int32)Ln, Rel, SrcLine))
				{
					Out->SetStringField(TEXT("rel"), Rel);
					Out->SetNumberField(TEXT("line"), SrcLine);
					Mapped.Add(MakeShared<FJsonValueObject>(Out));
				}
				else
				{
					Unmapped.Add(MakeShared<FJsonValueObject>(Out));
				}
			}
		}

		const bool bOk = (Status == 0) && Mapped.Num() == 0 && Unmapped.Num() == 0;
		if (bOk)
		{
			WritePending(TEXT("saved"), TEXT("Saved on Portal."));
		}
		else
		{
			WritePending(TEXT("built"), FString::Printf(TEXT("Portal refused the save: %s"),
				GrpcMessage.IsEmpty() ? *Toast : *GrpcMessage));
		}
		SendPending();

		TSharedRef<FJsonObject> O = MakeShared<FJsonObject>();
		O->SetStringField(TEXT("kind"), TEXT("portalverdict"));
		O->SetBoolField(TEXT("ok"), bOk);
		O->SetNumberField(TEXT("grpcStatus"), Status);
		O->SetStringField(TEXT("grpcMessage"), GrpcMessage);
		O->SetStringField(TEXT("toast"), Toast);
		O->SetStringField(TEXT("where"), Where);
		O->SetStringField(TEXT("at"), FDateTime::Now().ToString(TEXT("%H:%M:%S")));
		O->SetNumberField(TEXT("mapKnown"), GLineMap.Num());
		O->SetArrayField(TEXT("mapped"), Mapped);
		O->SetArrayField(TEXT("unmapped"), Unmapped);
		Event(O);

		UE_LOG(LogBF6Script, Display,
			TEXT("Portal verdict: status %d '%s'. %d marker%s mapped to source, %d not. Found by: %s"),
			Status, *GrpcMessage, Mapped.Num(), Mapped.Num() == 1 ? TEXT("") : TEXT("s"), Unmapped.Num(), *Where);
		return;
	}
}

// ---------------------------------------------------------------------------
// The session.
// ---------------------------------------------------------------------------
void BF6Script::NotifySessionLost(const FString& Why)
{
	GSiteOnScriptPage = false;
	// The page the tool knew is gone, so nothing may be identified as being on
	// it any more. Moving the generation is what makes a reply that arrives from
	// the old document after this point read as stale rather than as an answer
	// about whatever page the sign-in lands on.
	GSitePageExperience.Reset();
	GSitePageUrl.Reset();
	GSitePageGen++;
	if (GSiteReq != 0)
	{
		// A push or a save was in flight when the session went. Fail it in
		// words the user can act on rather than leaving the button spinning.
		const int32 Id = GSiteReq;
		GSiteReq = 0;
		GSiteOp.Reset();
		GSiteReqExperience.Reset();
		ReplyFail(Id, TEXT("Portal signed you out part way through. Nothing was lost: your script is on disk here. Sign in again on the Portal panel and press PUSH AGAIN."));
	}

	TSharedRef<FJsonObject> O = MakeShared<FJsonObject>();
	O->SetStringField(TEXT("kind"), TEXT("session"));
	O->SetStringField(TEXT("state"), TEXT("lost"));
	O->SetStringField(TEXT("why"), Why);
	Event(O);
	SendPending();
	UE_LOG(LogBF6Script, Warning, TEXT("Portal session lost (%s). The script project on disk is untouched."), *Why);
}

void BF6Script::NotifySessionRestored()
{
	TSharedRef<FJsonObject> O = MakeShared<FJsonObject>();
	O->SetStringField(TEXT("kind"), TEXT("session"));
	O->SetStringField(TEXT("state"), TEXT("restored"));
	O->SetStringField(TEXT("experience"), GExperienceId);
	Event(O);
	SendPending();
	UE_LOG(LogBF6Script, Display, TEXT("Portal session restored."));
}

void BF6Script::SimulateSession(const FString& What)
{
	if (What.StartsWith(TEXT("lost"))) { NotifySessionLost(TEXT("simulated from the console")); }
	else if (What.StartsWith(TEXT("restored"))) { NotifySessionRestored(); }
	else { UE_LOG(LogBF6Script, Warning, TEXT("Say lost or restored.")); }
}

// ---------------------------------------------------------------------------
// UBF6ScriptBridge
// ---------------------------------------------------------------------------
void UBF6ScriptBridge::ready(FString Json)
{
	TSharedPtr<FJsonObject> O = ParseJson(Json);
	FString Src;
	if (O.IsValid()) O->TryGetStringField(TEXT("src"), Src);
	if (Src == TEXT("site")) return;

	GPageReady = true;
	UE_LOG(LogBF6Script, Display, TEXT("Editor page ready."));
	SendStatus();
}

void UBF6ScriptBridge::call(FString Json)
{
	TSharedPtr<FJsonObject> O = ParseJson(Json);
	if (!O.IsValid())
	{
		UE_LOG(LogBF6Script, Warning, TEXT("A page sent something that is not JSON: %s"), *Json.Left(200));
		return;
	}
	FString Src, Op;
	O->TryGetStringField(TEXT("src"), Src);
	O->TryGetStringField(TEXT("op"), Op);

	if (Src == TEXT("site")) { HandleSiteCall(Op, O.ToSharedRef()); return; }

	double IdNum = 0;
	O->TryGetNumberField(TEXT("id"), IdNum);
	HandleEditorCall((int32)IdNum, Op, O.ToSharedRef());
}

void UBF6ScriptBridge::log(FString Json)
{
	TSharedPtr<FJsonObject> O = ParseJson(Json);
	if (!O.IsValid()) return;
	FString Level, Msg;
	O->TryGetStringField(TEXT("level"), Level);
	O->TryGetStringField(TEXT("msg"), Msg);
	// UE_LOG expands to a braced block, so each arm needs braces of its own or
	// the semicolon after it orphans the else.
	if (Level == TEXT("error")) { UE_LOG(LogBF6Script, Error, TEXT("page: %s"), *Msg); }
	else if (Level == TEXT("warn")) { UE_LOG(LogBF6Script, Warning, TEXT("page: %s"), *Msg); }
	else { UE_LOG(LogBF6Script, Display, TEXT("page: %s"), *Msg); }
}

// ---------------------------------------------------------------------------
// The tab.
// ---------------------------------------------------------------------------
static bool WebAvailable(FString& OutWhy)
{
	if (!FModuleManager::Get().IsModuleLoaded(TEXT("WebBrowser"))
		&& !FModuleManager::Get().LoadModule(TEXT("WebBrowser")))
	{
		OutWhy = TEXT("This engine build has no WebBrowser module, so the editor cannot be embedded.");
		return false;
	}
	if (!IWebBrowserModule::IsAvailable() || !IWebBrowserModule::Get().IsWebModuleAvailable())
	{
		OutWhy = TEXT("The browser back end is not available in this engine build.");
		return false;
	}
	return true;
}

static TSharedRef<SWidget> MakeUnavailableView()
{
	return SNew(SBorder)
		.BorderBackgroundColor(FSlateColor(BF6Theme::Panel))
		.Padding(20)
		[
			SNew(SVerticalBox)
			+ SVerticalBox::Slot().AutoHeight().Padding(0, 0, 0, 10)
			[
				SNew(STextBlock).ColorAndOpacity(FSlateColor(BF6Theme::Accent))
				.Text(FText::FromString(TEXT("THE SCRIPT EDITOR CANNOT BE EMBEDDED HERE")))
			]
			+ SVerticalBox::Slot().AutoHeight()
			[
				SNew(STextBlock).AutoWrapText(true).ColorAndOpacity(FSlateColor(BF6Theme::Text))
				.Text_Lambda([] { return FText::FromString(GUnavailableWhy); })
			]
		];
}

static TSharedRef<SWidget> MakeEditorView()
{
	if (!WebAvailable(GUnavailableWhy)) return MakeUnavailableView();

	IWebBrowserSingleton* S = IWebBrowserModule::Get().GetSingleton();
	if (!S)
	{
		GUnavailableWhy = TEXT("The browser back end did not start.");
		return MakeUnavailableView();
	}

	const FString Page = FPaths::Combine(ScriptRes(), TEXT("editor.html"));
	if (!FPaths::FileExists(Page))
	{
		GUnavailableWhy = FString::Printf(TEXT("The editor page is missing: %s"), *Page);
		return MakeUnavailableView();
	}

	FCreateBrowserWindowSettings St;
	// A FRESH URL EVERY TIME. CEF caches file:// resources, and a cached
	// editor.html means an edited page never loads at all - the host was
	// logging "telling the page to show dist/bundle.ts first" while the page
	// carried on opening src/index.ts, because the page in the browser was
	// older than the fix. The query changes nothing about which file is loaded
	// and makes serving yesterday's copy impossible.
	St.InitialURL = FileUrl(Page) + FString::Printf(TEXT("?v=%lld"),
		(long long)FDateTime::UtcNow().GetTicks());
	St.BackgroundColor = FColor(0x0D, 0x0F, 0x10);
	St.bUseTransparency = false;
	St.bShowErrorMessage = true;
	St.UserAgentApplication = FString(TEXT("BF6UnrealSDK"));

	GWindow = S->CreateBrowserWindow(St);
	if (!GWindow.IsValid())
	{
		GUnavailableWhy = TEXT("A browser window could not be created for the editor.");
		return MakeUnavailableView();
	}

	if (GBridge.IsValid()) GWindow->BindUObject(kBridgeName, GBridge.Get(), true);

	GBrowser = SNew(SWebBrowser, GWindow)
		.ShowControls(false)
		.ShowAddressBar(false)
		.ShowErrorMessage(true)
		.ShowInitialThrobber(true)
		.BackgroundColor(FColor(0x0D, 0x0F, 0x10))
		.OnLoadCompleted_Lambda([]
		{
			UE_LOG(LogBF6Script, Display, TEXT("Editor page loaded."));
		})
		.OnLoadError_Lambda([]
		{
			UE_LOG(LogBF6Script, Error, TEXT("The editor page did not load. Check that %s is there."),
				*FPaths::Combine(ScriptRes(), TEXT("editor.html")));
		})
		.OnConsoleMessage_Lambda([](const FString& Msg, const FString& Src, int32 Line, EWebBrowserConsoleLogSeverity Sev)
		{
			// Our own page announces itself; the rest is Monaco's noise and
			// stays at Verbose.
			if (Msg.StartsWith(TEXT("BF6")))
			{
				UE_LOG(LogBF6Script, Display, TEXT("%s"), *Msg);
			}
			else if (Sev == EWebBrowserConsoleLogSeverity::Error)
			{
				UE_LOG(LogBF6Script, Warning, TEXT("page error: %s (%s:%d)"), *Msg, *Src, Line);
			}
			else
			{
				UE_LOG(LogBF6Script, Verbose, TEXT("page: %s"), *Msg);
			}
		});

	UE_LOG(LogBF6Script, Display, TEXT("Editor window created on %s"), *St.InitialURL);
	return GBrowser.ToSharedRef();
}

// ---- BF6EditorOverlay ----
// THE PAGE IS MADE ONCE, and MakeEditorView already worked that way: this only
// gives the rule a name, so the tab and the full-screen host can both ask for
// the same widget without either of them knowing how it was built.
static TSharedRef<SWidget> EnsureEditorPage()
{
	if (GBrowser.IsValid()) return GBrowser.ToSharedRef();
	TSharedRef<SWidget> View = MakeEditorView();
	// No browser in this build: what comes back is the message page, and it is
	// shown as it is rather than swallowed for a blank panel.
	return GBrowser.IsValid() ? GBrowser.ToSharedRef() : View;
}
// ---- end BF6EditorOverlay ----

static TSharedRef<SDockTab> SpawnDockTab(const FSpawnTabArgs&)
{
	// ---- BF6EditorOverlay ----
	// A tab asking for the page takes it off the full-screen host: the two
	// placements share one widget, and whichever one is asked for wins.
	BF6EditorOverlay::HideIfShowing(BF6EditorOverlay::EEditor::Script);
	// ---- end BF6EditorOverlay ----
	if (!GHost.IsValid()) GHost = SNew(SBox);
	GHost->SetContent(EnsureEditorPage());

	TSharedRef<SDockTab> Tab = SNew(SDockTab)
		.TabRole(ETabRole::NomadTab)
		.Label(FText::FromString(TEXT("BF6 Script")));
	Tab->SetContent(GHost.ToSharedRef());
	GTab = Tab;
	return Tab;
}

// ---------------------------------------------------------------------------
// The public entry points.
// ---------------------------------------------------------------------------
// THE PANEL FOLLOWS THE EXPERIENCE YOU HAVE OPEN.
//
// Importing an experience scaffolds its script project and writes the site's
// bundle into it, and then nothing pointed the panel at it: the file list is
// built from whichever project happens to be open, so a freshly imported
// experience showed an empty panel while its dist/bundle.ts sat on disk a
// folder away. "None of the scripts show up" is exactly that.
//
// Adopted only when NOTHING is open, so a project somebody chose by hand is
// never swapped out from under them. A project is a folder with a package.json
// in it; anything else is a folder that happens to exist.
// ---------------------------------------------------------------------------
// A BUNDLE IS NOT SOURCE, AND GUESSING THE SOURCE BACK IS NOT GOOD ENOUGH.
//
// What Portal hands back is bf6-portal-bundler output: every module
// concatenated into one file with "@ts-nocheck" on top and EVERY IMPORT
// STRIPPED. It does keep a "// --- SOURCE: <path> ---" line before each module,
// so the file list and the directory layout are recoverable exactly.
//
// The imports are not. Reconstructing them by working out which file exports
// each name was tried and MEASURED against a real 19-file mod: the result
// compiled and re-bundled, but the bundle was 9,932 lines against the original
// 7,937, because guessing pulls library modules in that the original never
// used. It builds a different mod. So it is not what an import does by default.
//
// Somebody who has the experience open in the tool almost always has the
// project it was built from sitting on their disk. Asking for that folder takes
// one dialog and is exact, where reconstruction is a plausible-looking
// approximation. So the tool asks.
// ---------------------------------------------------------------------------

// Is this file bundler output, and how many modules did it come from?
bool BF6Script::LooksBundled(const FString& BundlePath, int32& OutOwnModules)
{
	OutOwnModules = 0;
	FString Text;
	if (!FFileHelper::LoadFileToString(Text, *BundlePath)) return false;
	if (!Text.StartsWith(TEXT("// --- BUNDLED TYPESCRIPT OUTPUT ---"))) return false;

	TArray<FString> Lines;
	Text.ParseIntoArrayLines(Lines, false);
	for (const FString& L : Lines)
	{
		if (!L.StartsWith(TEXT("// --- SOURCE:"))) continue;
		// A module out of node_modules is a dependency npm already provides;
		// only the author's own files count towards "is this worth splitting".
		if (L.Contains(TEXT("node_modules"))) continue;
		OutOwnModules++;
	}
	return OutOwnModules > 0;
}

// The names of the author's own modules, as the bundle records them. Used to
// tell somebody what the tool is looking for before it asks for a folder.
void BF6Script::BundledModuleNames(const FString& BundlePath, TArray<FString>& Out)
{
	Out.Reset();
	FString Text;
	if (!FFileHelper::LoadFileToString(Text, *BundlePath)) return;
	TArray<FString> Lines;
	Text.ParseIntoArrayLines(Lines, false);
	for (const FString& L : Lines)
	{
		if (!L.StartsWith(TEXT("// --- SOURCE:"))) continue;
		if (L.Contains(TEXT("node_modules"))) continue;
		FString Rel = L;
		Rel.RemoveFromStart(TEXT("// --- SOURCE:"));
		Rel.RemoveFromEnd(TEXT("---"));
		Rel = Rel.TrimStartAndEnd();
		Rel.RemoveFromEnd(TEXT(" ---"));
		Rel.ReplaceInline(TEXT("\\"), TEXT("/"));
		if (!Rel.IsEmpty()) Out.Add(Rel);
	}
}

// Copy the author's real source over the project's src/. Exact, because it IS
// their source rather than a reconstruction of it.
bool BF6Script::UseSourceFolder(const FString& ProjectDir, const FString& SrcFolder, FString& OutWhy)
{
	if (!FPaths::DirectoryExists(SrcFolder))
	{
		OutWhy = FString::Printf(TEXT("There is no folder at %s."), *SrcFolder);
		return false;
	}
	IFileManager& FM = IFileManager::Get();
	TArray<FString> Files;
	FM.FindFilesRecursive(Files, *SrcFolder, TEXT("*"), true, false);
	if (Files.Num() == 0)
	{
		OutWhy = FString::Printf(TEXT("%s has no files in it."), *SrcFolder);
		return false;
	}

	const FString Dest = FPaths::Combine(ProjectDir, TEXT("src"));
	int32 Copied = 0;
	for (const FString& F : Files)
	{
		const FString Ext = FPaths::GetExtension(F).ToLower();
		// Source and its data only. node_modules and build output are not
		// somebody's source even when they are sitting in the folder.
		if (Ext != TEXT("ts") && Ext != TEXT("json") && Ext != TEXT("png") && Ext != TEXT("jpg")) continue;
		if (F.Contains(TEXT("node_modules")) || F.Contains(TEXT("/dist/")) || F.Contains(TEXT("\\dist\\"))) continue;
		FString Rel = F;
		FPaths::MakePathRelativeTo(Rel, *(SrcFolder / TEXT("")));
		const FString To = FPaths::Combine(Dest, Rel);
		IFileManager::Get().MakeDirectory(*FPaths::GetPath(To), true);
		// The template's starter files are replaced on purpose; anything else
		// already there is kept beside itself rather than lost.
		if (FPaths::FileExists(To))
		{
			FM.Copy(*(To + TEXT(".orig")), *To);
		}
		if (FM.Copy(*To, *F) == COPY_OK) Copied++;
	}
	if (Copied == 0)
	{
		OutWhy = FString::Printf(TEXT("Nothing in %s looked like script source."), *SrcFolder);
		return false;
	}
	MarkSourceAdopted(ProjectDir, FString::Printf(TEXT("your own source from %s"), *SrcFolder));
	BF6Script::ClearBundleImported(ProjectDir);
	UE_LOG(LogBF6Script, Display,
		TEXT("Script: copied %d file(s) of your own source into %s. This project now builds from it."),
		Copied, *Dest);
	return true;
}

// ---------------------------------------------------------------------------
// PUBLISH MODE: the upload without the workbench.
//
// The template brings a debug tool and a logging module. They are worth having
// while building a mod and are dead weight on the site, so publish mode leaves
// them out of the bundle that gets uploaded.
//
// WHAT THIS IS NOT. It is not "strip the template", because most of what the
// template contributes is not scaffolding: measured on a real 300 KB mod, the
// 1,304 lines of bf6-portal-utils in it were events, ui, raycast, sounds and
// timers - all of them CALLED by the mod. Removing those does not slim a mod,
// it breaks one. The bundler already leaves out whatever is never imported, so
// the only honest saving is the workbench that a finished mod stops using.
//
// AND IT IS VERIFIED, NOT ASSUMED. The lean copy is built by the real bundler
// before it is allowed anywhere near an upload. If the mod still calls what was
// removed, the build fails, and a failed build is reported instead of a broken
// upload. That check is the whole reason this is safe to offer.
//
// The lean copy lives INSIDE the project, at .bf6-publish/, so node resolves
// node_modules by walking up to the project root exactly as a normal build
// does; nothing has to be copied but the source.
// ---------------------------------------------------------------------------
static const TCHAR* kPublishDir = TEXT(".bf6-publish");

// WHAT PUBLISH MODE CAN HONESTLY REMOVE.
//
// The first version copied src minus the debug-tool folder into .bf6-publish
// and nothing ever compiled that copy: the build went on using the project's
// own tsconfig and its own src, so publish mode changed nothing at all while
// the log said "the bundle is made from that copy". It could not have worked
// either. The bundler follows imports from the entry point, so a file that is
// not imported never reaches the bundle, and a file that IS imported cannot be
// deleted from a copy without that copy failing to compile.
//
// So the strip happens where the content actually is: in the built bundle,
// which the bundler helpfully divides into modules with
//   // --- SOURCE: src\debug-tool\index.ts ---
// markers. A module is removed only when NOTHING outside it uses any of the
// names it declares. That is a proof rather than a hope: if the mod calls the
// debug tool, the check fails, everything is kept, and the user is told which
// name is holding it in.
static const TCHAR* kSourceMarker = TEXT("// --- SOURCE: ");

struct FBundleModule
{
	FString Path;       // as the bundler wrote it
	int32   FirstLine = 0;
	int32   LastLine = 0;
};

static TArray<FBundleModule> SplitBundleModules(const TArray<FString>& Lines)
{
	TArray<FBundleModule> Out;
	for (int32 i = 0; i < Lines.Num(); i++)
	{
		if (!Lines[i].StartsWith(kSourceMarker)) { continue; }
		if (Out.Num() > 0) { Out.Last().LastLine = i - 1; }
		FBundleModule M;
		M.FirstLine = i;
		M.Path = Lines[i].Mid(FCString::Strlen(kSourceMarker));
		M.Path.RemoveFromEnd(TEXT(" ---"));
		M.Path.TrimStartAndEndInline();
		Out.Add(M);
	}
	if (Out.Num() > 0) { Out.Last().LastLine = Lines.Num() - 1; }
	return Out;
}

static bool IsWorkbenchModule(const FString& Path)
{
	FString P = Path;
	P.ReplaceInline(TEXT("\\"), TEXT("/"));
	return P.Contains(TEXT("/debug-tool/")) || P.StartsWith(TEXT("debug-tool/"));
}

// The top-level names a module declares. Only these can be referenced from
// elsewhere in a bundle, because the bundler flattens every module into the
// same scope.
static void DeclaredNames(const TArray<FString>& Lines, const FBundleModule& M, TSet<FString>& Out)
{
	for (int32 i = M.FirstLine; i <= M.LastLine && i < Lines.Num(); i++)
	{
		const FString& L = Lines[i];
		static const TCHAR* kHeads[] = {
			TEXT("function "), TEXT("class "), TEXT("const "), TEXT("let "), TEXT("var "),
			TEXT("enum "), TEXT("interface "), TEXT("type "), TEXT("abstract class ")
		};
		FString T = L;
		T.TrimStartInline();
		T.RemoveFromStart(TEXT("export "));
		T.RemoveFromStart(TEXT("declare "));
		T.RemoveFromStart(TEXT("async "));
		for (const TCHAR* Head : kHeads)
		{
			if (!T.StartsWith(Head)) { continue; }
			FString Rest = T.Mid(FCString::Strlen(Head));
			Rest.TrimStartInline();
			int32 End = 0;
			while (End < Rest.Len() && (FChar::IsAlnum(Rest[End]) || Rest[End] == TEXT('_')
				   || Rest[End] == TEXT('$'))) { End++; }
			if (End > 0) { Out.Add(Rest.Left(End)); }
			break;
		}
	}
}

// Is this name used anywhere outside the modules being removed?
static bool NameUsedOutside(const TArray<FString>& Lines, const TArray<FBundleModule>& Drop,
                            const FString& Name)
{
	auto InDropped = [&Drop](int32 Line)
	{
		for (const FBundleModule& M : Drop)
		{
			if (Line >= M.FirstLine && Line <= M.LastLine) { return true; }
		}
		return false;
	};
	for (int32 i = 0; i < Lines.Num(); i++)
	{
		if (InDropped(i)) { continue; }
		const FString& L = Lines[i];
		int32 At = 0;
		while (true)
		{
			const int32 Found = L.Find(Name, ESearchCase::CaseSensitive, ESearchDir::FromStart, At);
			if (Found == INDEX_NONE) { break; }
			const TCHAR Before = Found > 0 ? L[Found - 1] : TEXT(' ');
			const int32 AfterAt = Found + Name.Len();
			const TCHAR After = AfterAt < L.Len() ? L[AfterAt] : TEXT(' ');
			const bool bWordBefore = FChar::IsAlnum(Before) || Before == TEXT('_') || Before == TEXT('$');
			const bool bWordAfter  = FChar::IsAlnum(After)  || After  == TEXT('_') || After  == TEXT('$');
			if (!bWordBefore && !bWordAfter) { return true; }
			At = Found + 1;
		}
	}
	return false;
}

bool BF6Script::MakeLeanBundle(const FString& ProjectDir, FString& OutWhy, int32& OutLinesRemoved)
{
	OutLinesRemoved = 0;
	const FString BundlePath = FPaths::Combine(ProjectDir, TEXT("dist"), TEXT("bundle.ts"));
	FString Text;
	if (!FFileHelper::LoadFileToString(Text, *BundlePath))
	{
		OutWhy = TEXT("there is no dist/bundle.ts to make a publish copy from");
		return false;
	}

	TArray<FString> Lines;
	Text.ParseIntoArray(Lines, TEXT("\n"), false);
	for (FString& L : Lines) { L.RemoveFromEnd(TEXT("\r")); }

	const TArray<FBundleModule> Modules = SplitBundleModules(Lines);
	TArray<FBundleModule> Drop;
	for (const FBundleModule& M : Modules)
	{
		if (IsWorkbenchModule(M.Path)) { Drop.Add(M); }
	}

	const FString OutPath = FPaths::Combine(ProjectDir, kPublishDir, TEXT("dist"), TEXT("bundle.ts"));
	IFileManager::Get().MakeDirectory(*FPaths::GetPath(OutPath), true);

	if (Drop.Num() == 0)
	{
		// Nothing to take out. The copy is still written, because the push
		// reads whatever publish mode produced and a missing file would be a
		// silent fall back to the ordinary bundle.
		if (!FFileHelper::SaveStringToFile(Text, *OutPath,
			FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM))
		{
			OutWhy = TEXT("the publish copy could not be written");
			return false;
		}
		UE_LOG(LogBF6Script, Display,
			TEXT("Publish build: the bundle contains no workbench code, so the published file is the same as the built one."));
		return true;
	}

	TSet<FString> Names;
	for (const FBundleModule& M : Drop) { DeclaredNames(Lines, M, Names); }

	TArray<FString> Held;
	for (const FString& N : Names)
	{
		if (NameUsedOutside(Lines, Drop, N)) { Held.Add(N); }
	}
	if (Held.Num() > 0)
	{
		Held.Sort();
		OutWhy = FString::Printf(
			TEXT("your mod uses the debug tool, so leaving it out would break the upload. ")
			TEXT("These names are used outside it: %s"),
			*FString::Join(Held, TEXT(", ")));
		return false;
	}

	TArray<FString> Kept;
	Kept.Reserve(Lines.Num());
	for (int32 i = 0; i < Lines.Num(); i++)
	{
		bool bDrop = false;
		for (const FBundleModule& M : Drop)
		{
			if (i >= M.FirstLine && i <= M.LastLine) { bDrop = true; break; }
		}
		if (bDrop) { OutLinesRemoved++; continue; }
		Kept.Add(Lines[i]);
	}

	if (!FFileHelper::SaveStringToFile(FString::Join(Kept, TEXT("\n")), *OutPath,
		FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM))
	{
		OutWhy = TEXT("the publish copy could not be written");
		return false;
	}
	UE_LOG(LogBF6Script, Display,
		TEXT("Publish build: %d line(s) of workbench code left out of the published bundle; ")
		TEXT("nothing outside it used any of its %d name(s)."), OutLinesRemoved, Names.Num());
	return true;
}


FString BF6Script::LeanBundlePath(const FString& ProjectDir)
{
	return FPaths::Combine(ProjectDir, kPublishDir, TEXT("dist"), TEXT("bundle.ts"));
}

bool BF6Script::PublishMode(const FString& ProjectDir)
{
	FString Text;
	if (!FFileHelper::LoadFileToString(Text,
		*FPaths::Combine(ProjectDir, TEXT("bf6-publish-mode.txt")))) return false;
	return Text.TrimStartAndEnd() == TEXT("1");
}

void BF6Script::SetPublishMode(const FString& ProjectDir, bool bOn)
{
	// A file rather than a setting, so it travels with the project: the person
	// who opens this folder next gets the same answer about what ships.
	FFileHelper::SaveStringToFile(bOn ? TEXT("1") : TEXT("0"),
		*FPaths::Combine(ProjectDir, TEXT("bf6-publish-mode.txt")),
		FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
	UE_LOG(LogBF6Script, Display,
		TEXT("Publish mode %s for %s."), bOn ? TEXT("on") : TEXT("off"),
		*FPaths::GetCleanFilename(ProjectDir));
}

void BF6Script::ShowFileFirst(const FString& Rel)
{
	GPreferredOpen = Rel;
}

// ---------------------------------------------------------------------------
// PLAYING A PLACEABLE'S SOUND WITHOUT THE ADD-ON.
//
// The object library's Play button is served by whichever preview is
// registered. The High Poly add-on registers one that reads the game itself,
// which is the better answer and wins whenever it is installed - add-ons load
// after this module, so its registration simply replaces the one below.
//
// This is the other half: a folder of clips recorded from the game. The two
// editors can hand an ogg to a browser and let it play; the object library is
// Slate and has no browser, so the bytes have to become a sound wave here.
// That is what FVorbisAudioInfo is for: it is the same decoder the engine uses
// for its own cooked audio, so there is no third-party decoder to vendor.
//
// Decoded once per sound and kept, because a library row can be clicked
// repeatedly and decoding 6 seconds of audio for each click would be felt.
// ---------------------------------------------------------------------------
class FSoundboardPreview : public BF6Ext::IPlaceableSoundPreview
{
public:
	virtual ~FSoundboardPreview() { Stop(); }

	virtual bool CanPreview(const FString& PlaceableType) const override
	{
		// Answered from the index, never by touching the disk: this is asked
		// while every row of the library is built.
		return Index().Contains(PlaceableType);
	}

	virtual void Preview(const FString& PlaceableType) override
	{
		if (Playing_ == PlaceableType) { Stop(); return; }
		Stop();
		const FString* Rel = Index().Find(PlaceableType);
		if (!Rel) { return; }

		USoundWave* Wave = WaveFor(PlaceableType, *Rel);
		if (!Wave) { return; }
		if (!GEditor) { return; }
		Playing_ = PlaceableType;
		Wave->AddToRoot();
		Held = Wave;
		GEditor->PlayPreviewSound(Wave);
	}

	virtual void Stop() override
	{
		if (GEditor) { GEditor->ResetPreviewAudioComponent(); }
		if (Held) { Held->RemoveFromRoot(); Held = nullptr; }
		Playing_.Reset();
	}

	virtual FString Playing() const override { return Playing_; }

private:
	FString Playing_;
	USoundWave* Held = nullptr;
	mutable TMap<FString, FString> Rows;      // placeable -> clip, relative
	mutable bool bIndexed = false;
	TMap<FString, TWeakObjectPtr<USoundWave>> Cache;

	const TMap<FString, FString>& Index() const
	{
		if (bIndexed) { return Rows; }
		bIndexed = true;
		const FString SB = SoundboardDir();
		if (SB.IsEmpty()) { return Rows; }
		FString Text;
		if (!FFileHelper::LoadFileToString(Text, *FPaths::Combine(SB, TEXT("manifest.json")))) { return Rows; }
		TArray<TSharedPtr<FJsonValue>> Arr;
		const TSharedRef<TJsonReader<>> R = TJsonReaderFactory<>::Create(Text);
		if (!FJsonSerializer::Deserialize(R, Arr)) { return Rows; }
		for (const TSharedPtr<FJsonValue>& V : Arr)
		{
			const TSharedPtr<FJsonObject>* O = nullptr;
			if (!V.IsValid() || !V->TryGetObject(O) || !O) { continue; }
			FString Name, File;
			bool bCrash = false;
			(*O)->TryGetStringField(TEXT("name"), Name);
			(*O)->TryGetStringField(TEXT("file"), File);
			(*O)->TryGetBoolField(TEXT("crash"), bCrash);
			// Two of these are known to crash the GAME. They do not crash the
			// editor, but offering one from a library row is still handing
			// somebody a landmine, so they are left out of the index entirely.
			if (Name.IsEmpty() || File.IsEmpty() || bCrash) { continue; }
			Rows.Add(Name, File);
		}
		UE_LOG(LogBF6, Display,
			TEXT("Sound library: %d placeable sound(s) can be previewed from %s"), Rows.Num(), *SB);
		return Rows;
	}

	USoundWave* WaveFor(const FString& Name, const FString& Rel)
	{
		if (TWeakObjectPtr<USoundWave>* Found = Cache.Find(Name))
		{
			if (Found->IsValid()) { return Found->Get(); }
		}
		TArray<uint8> Ogg;
		if (!FFileHelper::LoadFileToArray(Ogg, *FPaths::Combine(SoundboardDir(), Rel))) { return nullptr; }

		FVorbisAudioInfo Info;
		FSoundQualityInfo Quality;
		if (!Info.ReadCompressedInfo(Ogg.GetData(), Ogg.Num(), &Quality))
		{
			UE_LOG(LogBF6, Warning, TEXT("Sound library: %s is not readable as vorbis."), *Name);
			return nullptr;
		}
		TArray<uint8> Pcm;
		Pcm.SetNumUninitialized(Quality.SampleDataSize);
		Info.ExpandFile(Pcm.GetData(), &Quality);

		USoundWaveProcedural* W = NewObject<USoundWaveProcedural>();
		if (!W) { return nullptr; }
		W->SetSampleRate(Quality.SampleRate);
		W->NumChannels = Quality.NumChannels;
		W->Duration = Quality.Duration;
		W->SoundGroup = SOUNDGROUP_Default;
		W->bLooping = false;
		W->QueueAudio(Pcm.GetData(), Pcm.Num());
		Cache.Add(Name, W);
		return W;
	}
};

// The object library's Play button, served from the recorded library when no
// add-on is there to serve it better. Registered at startup; an add-on that
// loads afterwards replaces it, which is the order we want.
void BF6Script::RegisterSoundboardPreview()
{
	if (SoundboardDir().IsEmpty()) { return; }
	BF6Ext::RegisterPlaceableSoundPreview(MakeShared<FSoundboardPreview>());
	UE_LOG(LogBF6Script, Display,
		TEXT("Placeable sounds can be previewed from the recorded library. ")
		TEXT("Installing the High Poly add-on plays them from the game instead."));
}

FString BF6Script::SoundPreviewSource() { return PreviewSource(); }

bool BF6Script::PlayPlaceableSound(const FString& Name, FString& OutWhy)
{
	if (Name.IsEmpty()) { OutWhy = TEXT("No sound named."); return false; }
	if (!BF6UiSound::CanPreviewPlaceable(Name))
	{
		OutWhy = FString::Printf(TEXT("%s cannot be played from the install here."), *Name);
		return false;
	}
	BF6UiSound::TogglePreviewPlaceable(Name);
	return true;
}

void BF6Script::StopPlaceableSound() { BF6UiSound::StopPreview(); }

bool BF6Script::LoadSoundClipBase64(const FString& Name, FString& OutBase64, FString& OutWhy)
{
	const FString SB = SoundboardDir();
	if (SB.IsEmpty() || Name.IsEmpty()) { OutWhy = TEXT("There is no sound library to play from."); return false; }
	FString Text, Rel;
	if (FFileHelper::LoadFileToString(Text, *FPaths::Combine(SB, TEXT("manifest.json"))))
	{
		TArray<TSharedPtr<FJsonValue>> Arr;
		const TSharedRef<TJsonReader<>> R = TJsonReaderFactory<>::Create(Text);
		if (FJsonSerializer::Deserialize(R, Arr))
		{
			for (const TSharedPtr<FJsonValue>& V : Arr)
			{
				const TSharedPtr<FJsonObject>* O = nullptr;
				if (!V.IsValid() || !V->TryGetObject(O) || !O) continue;
				FString N;
				if ((*O)->TryGetStringField(TEXT("name"), N) && N == Name)
				{
					(*O)->TryGetStringField(TEXT("file"), Rel);
					break;
				}
			}
		}
	}
	if (Rel.IsEmpty()) { OutWhy = FString::Printf(TEXT("No clip for %s."), *Name); return false; }
	TArray<uint8> Bytes;
	if (!FFileHelper::LoadFileToArray(Bytes, *FPaths::Combine(SB, Rel)))
	{
		OutWhy = FString::Printf(TEXT("Could not read the clip for %s."), *Name);
		return false;
	}
	OutBase64 = FBase64::Encode(Bytes);
	return true;
}

bool BF6Script::OpenProjectAt(const FString& Dir)
{
	if (Dir.IsEmpty() || !FPaths::FileExists(FPaths::Combine(Dir, TEXT("package.json"))))
	{
		return false;
	}
	if (GProjectDir == Dir) { return true; }
	OpenProject(Dir);
	UE_LOG(LogBF6Script, Display, TEXT("Script: showing the project that was just imported: %s"), *Dir);
	return true;
}

static bool AdoptProjectForOpenSave()
{
	if (!GProjectDir.IsEmpty()) return false;
	const FString Dir = BF6Script::ProjectDirForSave(BF6Api::CurrentLevel(), BF6Api::CurrentSave());
	if (Dir.IsEmpty() || !FPaths::FileExists(FPaths::Combine(Dir, TEXT("package.json")))) return false;
	OpenProject(Dir);
	UE_LOG(LogBF6Script, Display,
		TEXT("Script: opened the project for the experience you have open: %s"), *Dir);
	return true;
}

void BF6Script::Open()
{
	// ---- BF6EditorOverlay ----
	// Asking for the tab moves the page into it, so the host must let go first.
	BF6EditorOverlay::HideIfShowing(BF6EditorOverlay::EEditor::Script);
	// ---- end BF6EditorOverlay ----
	TSharedPtr<SDockTab> Tab = FGlobalTabmanager::Get()->TryInvokeTab(FTabId(kTabId));
	if (!Tab.IsValid())
	{
		UE_LOG(LogBF6Script, Warning, TEXT("The BF6 Script tab could not be opened."));
		return;
	}
	// Before the page is filled, so the first thing it draws is the right
	// project rather than an empty list somebody has to fix by hand.
	AdoptProjectForOpenSave();
	// A tab that was already open does not spawn again, so fill it here too.
	if (GHost.IsValid()) GHost->SetContent(EnsureEditorPage());
}

// ---- BF6EditorOverlay ----
TSharedRef<SWidget> BF6Script::Widget()
{
	TSharedRef<SWidget> W = EnsureEditorPage();
	// One parent only: the dock tab gives it up while the host holds it, and
	// the tab is left empty rather than closed, so it is still there to go
	// back into.
	if (GHost.IsValid()) GHost->SetContent(SNullWidget::NullWidget);
	return W;
}

void BF6Script::ReleaseWidget()
{
	if (GHost.IsValid() && GTab.IsValid()) GHost->SetContent(EnsureEditorPage());
}
// ---- end BF6EditorOverlay ----

void BF6Script::NewProject(const FString& Name)
{
	Open();
	if (Name.IsEmpty())
	{
		ExecEditor(TEXT("try{document.getElementById('btnNew').click()}catch(e){}"));
		return;
	}
	TSharedRef<FJsonObject> O = MakeShared<FJsonObject>();
	O->SetStringField(TEXT("name"), Name);
	O->SetStringField(TEXT("boilerplate"), TEXT("minimal"));
	FString Why;
	const FString Dir = FPaths::Combine(ScriptSaved(), KebabCase(Name));
	if (FPaths::DirectoryExists(Dir))
	{
		UE_LOG(LogBF6Script, Warning, TEXT("There is already a project at %s."), *Dir);
		return;
	}
	// No experience id: this project lives in the unlinked folder and belongs to
	// no experience until somebody links it. See the same note in the "new" op.
	if (!CopyTemplate(Dir, Why) || !ApplyInit(Dir, Name, FString(), TEXT("minimal"), FString(), Why))
	{
		UE_LOG(LogBF6Script, Error, TEXT("%s"), *Why);
		return;
	}
	OpenProject(Dir);
	SendStatus();
	Notify(FString::Printf(TEXT("Script project created: %s"), *Dir));
}

// One file from somewhere else into this project. The block editor hands over
// the .ts and strings attachments of an imported experience; a person can hand
// over anything. Nothing is overwritten silently: a name already in use gets a
// numbered sibling and the log says which file was written.
bool BF6Script::ImportFile(const FString& Path)
{
	if (!FPaths::FileExists(Path))
	{
		UE_LOG(LogBF6Script, Warning, TEXT("Nothing to import: %s does not exist."), *Path);
		return false;
	}
	if (GProjectDir.IsEmpty())
	{
		// No project open: the one belonging to the open save will do, if it
		// has been created. Creating one here would be a surprise.
		const FString Dir = BF6Script::ProjectDirForSave(BF6Api::CurrentLevel(), BF6Api::CurrentSave());
		if (!Dir.IsEmpty() && FPaths::FileExists(FPaths::Combine(Dir, TEXT("package.json"))))
		{
			OpenProject(Dir);
		}
		else
		{
			UE_LOG(LogBF6Script, Warning,
				TEXT("No script project is open, so %s has nowhere to go. Open or create one first."),
				*FPaths::GetCleanFilename(Path));
			return false;
		}
	}

	const FString Name = FPaths::GetCleanFilename(Path);
	const FString Ext  = FPaths::GetExtension(Name).ToLower();
	FString Leaf = Name;
	if (Ext == TEXT("json") && Name.Contains(TEXT("strings"))) Leaf = TEXT("strings.json");

	FString Target = FPaths::Combine(GProjectDir, TEXT("src"), Leaf);
	if (FPaths::FileExists(Target))
	{
		const FString Base = FPaths::GetBaseFilename(Leaf);
		for (int32 n = 2; n < 100; ++n)
		{
			const FString Try = FPaths::Combine(GProjectDir, TEXT("src"),
				FString::Printf(TEXT("%s_%d.%s"), *Base, n, *Ext));
			if (!FPaths::FileExists(Try)) { Target = Try; break; }
		}
	}

	IFileManager::Get().MakeDirectory(*FPaths::GetPath(Target), true);
	if (IFileManager::Get().Copy(*Target, *Path) != COPY_OK)
	{
		UE_LOG(LogBF6Script, Warning, TEXT("Could not write %s"), *Target);
		return false;
	}

	UE_LOG(LogBF6Script, Display, TEXT("Imported %s into the project as %s"),
		*Name, *FPaths::GetCleanFilename(Target));
	SendStatus();
	Notify(FString::Printf(TEXT("Imported %s"), *FPaths::GetCleanFilename(Target)));
	return true;
}

void BF6Script::Build()
{
	// The toolbar and the console command go through the checked build like
	// everything else. There is exactly one way to produce a bundle here.
	FString Why;
	if (!StartCheckedBuild(0, Why))
	{
		UE_LOG(LogBF6Script, Warning, TEXT("%s"), *Why);
	}
}

bool BF6Script::HasProject()
{
	return !GProjectDir.IsEmpty();
}

void BF6Script::LogStatus()
{
	UE_LOG(LogBF6Script, Display, TEXT("---- BF6 Script ----"));
	UE_LOG(LogBF6Script, Display, TEXT("node:      %s"), GNodeExe.IsEmpty() ? TEXT("not found") : *GNodeExe);
	UE_LOG(LogBF6Script, Display, TEXT("npm:       %s"), GNpmCli.IsEmpty() ? TEXT("not found") : *GNpmCli);
	UE_LOG(LogBF6Script, Display, TEXT("template:  %s"), GTemplateDir.IsEmpty() ? TEXT("not found") : *GTemplateDir);
	UE_LOG(LogBF6Script, Display, TEXT("project:   %s"), GProjectDir.IsEmpty() ? TEXT("none open") : *GProjectDir);
	// The two destinations, side by side, because a push refuses whenever they
	// disagree and this is where somebody looks to find out why.
	UE_LOG(LogBF6Script, Display, TEXT("goes to:   %s"),
		GExperienceId.IsEmpty() ? TEXT("nowhere; this project is not linked to an experience") : *GExperienceId);
	UE_LOG(LogBF6Script, Display, TEXT("panel on:  %s"),
		GSitePageExperience.IsEmpty() ? TEXT("no experience the tool can name") : *GSitePageExperience);
	{
		FString Why;
		const bool bReady = !GProjectDir.IsEmpty() && ReadyToSend(Why);
		UE_LOG(LogBF6Script, Display, TEXT("build:     %s"),
			bReady ? *FString::Printf(TEXT("checked and ready to send (checked %s)"), *GCheckedAt)
			       : (GProjectDir.IsEmpty() ? TEXT("no project open") : *Why));
	}
	UE_LOG(LogBF6Script, Display, TEXT("page:      %s"), GPageReady ? TEXT("ready") : TEXT("not loaded"));
	UE_LOG(LogBF6Script, Display, TEXT("portal:    %s"), GSiteState.IsEmpty() ? TEXT("no page report yet") : *GSiteState);
	UE_LOG(LogBF6Script, Display, TEXT("log tail:  %s"), GTailing ? *GTailPath : TEXT("off"));
	UE_LOG(LogBF6Script, Display, TEXT("running:   %s"), GRun.IsValid() ? *GRun->Label : TEXT("nothing"));
}

// ---------------------------------------------------------------------------
// Register / Unregister
// ---------------------------------------------------------------------------
void BF6Script::Register()
{
	FindNode();
	FindTemplate();
	IFileManager::Get().MakeDirectory(*GeneratedDir(), true);

	// The object library's Play button, from the recorded library. Registered
	// here rather than lazily, because the library asks CanPreview while it
	// builds each row and a provider that appears later would leave the first
	// rows drawn without a button. An add-on loading afterwards replaces it.
	RegisterSoundboardPreview();

	GBridge.Reset(NewObject<UBF6ScriptBridge>(GetTransientPackage(), FName(TEXT("BF6ScriptBridge"))));

	// The same object on both windows: the editor asks, the site half answers.
	BF6PortalWeb::RegisterBridgeObject(kBridgeName, GBridge.Get());

	// The site half, off disk, so it can be iterated on without a recompile.
	const FString SyncPath = FPaths::Combine(ScriptRes(), TEXT("site_script_sync.js"));
	FString Sync;
	if (FFileHelper::LoadFileToString(Sync, *SyncPath))
	{
		BF6PortalWeb::RegisterInjectedScript(kSiteScriptId, Sync);
		UE_LOG(LogBF6Script, Display, TEXT("Portal sync script injected from %s (%d chars)"), *SyncPath, Sync.Len());
	}
	else
	{
		UE_LOG(LogBF6Script, Error, TEXT("The Portal sync script is missing: %s. PUSH and PULL cannot work without it."), *SyncPath);
	}

	FGlobalTabmanager::Get()->RegisterNomadTabSpawner(kTabId, FOnSpawnTab::CreateStatic(&SpawnDockTab))
		.SetDisplayName(FText::FromString(TEXT("BF6 Script")))
		.SetTooltipText(FText::FromString(TEXT("Write your experience's TypeScript here, with autocomplete, an explanation beside every line, and a push straight into the Portal page.")))
		.SetGroup(WorkspaceMenu::GetMenuStructure().GetToolsCategory());

	GCmds.Add(IConsoleManager::Get().RegisterConsoleCommand(TEXT("BF6.Script.Open"),
		TEXT("Open the BF6 Script tab."),
		FConsoleCommandDelegate::CreateStatic(&BF6Script::Open)));

	GCmds.Add(IConsoleManager::Get().RegisterConsoleCommand(TEXT("BF6.Script.PublishMode"),
		TEXT("BF6.Script.PublishMode [0|1]  Leave the template's debug tool out of what gets ")
		TEXT("uploaded. No argument reports the current setting and what it would save. ")
		TEXT("Turn it off again at any time; nothing in your project is changed either way."),
		FConsoleCommandWithArgsDelegate::CreateLambda([](const TArray<FString>& Args)
		{
			if (!BF6Script::HasProject())
			{
				UE_LOG(LogBF6Script, Warning, TEXT("Open a script project first."));
				return;
			}
			const FString Dir = GProjectDir;
			if (Args.Num() >= 1)
			{
				const FString A = Args[0].ToLower();
				const bool bOn = (A == TEXT("1") || A == TEXT("on") || A == TEXT("true"));
				BF6Script::SetPublishMode(Dir, bOn);
				if (!bOn)
				{
					UE_LOG(LogBF6Script, Display,
						TEXT("Uploads will carry the whole project again."));
					return;
				}
			}
			const bool bOn = BF6Script::PublishMode(Dir);
			UE_LOG(LogBF6Script, Display, TEXT("Publish mode is %s."), bOn ? TEXT("ON") : TEXT("off"));
			if (!bOn) { return; }

			// Reported against the bundle that exists NOW, so this says what
			// would actually be uploaded rather than what a copy of the source
			// might contain. A build has to have run for there to be one.
			int32 Removed = 0;
			FString Why;
			if (!BF6Script::MakeLeanBundle(Dir, Why, Removed))
			{
				UE_LOG(LogBF6Script, Warning, TEXT("Publish copy not made: %s"), *Why);
				return;
			}
			if (Removed == 0)
			{
				// Said plainly rather than left as an impressive-looking zero:
				// a mod that never wired the debug tool in has nothing here to
				// save, and the bundler had already left it out.
				UE_LOG(LogBF6Script, Display,
					TEXT("Nothing to leave out: the built bundle carries no workbench code, ")
					TEXT("so the published file is the same as the built one."));
			}
			else
			{
				UE_LOG(LogBF6Script, Display,
					TEXT("Publish copy made: %d line(s) of workbench code left out. ")
					TEXT("PUSH sends that copy while publish mode is on."), Removed);
			}
		})));

	GCmds.Add(IConsoleManager::Get().RegisterConsoleCommand(TEXT("BF6.Script.UseMySource"),
		TEXT("BF6.Script.UseMySource [folder]  Bring your own script source into the open project, ")
		TEXT("replacing the one bundled file an import brings back. No folder opens a picker. ")
		TEXT("This is exact, where splitting the bundle up can only ever be a guess: bundling ")
		TEXT("strips every import, and reconstructing them produced a bundle 2,000 lines larger ")
		TEXT("than the original on a real mod."),
		FConsoleCommandWithArgsDelegate::CreateLambda([](const TArray<FString>& Args)
		{
			if (!BF6Script::HasProject())
			{
				UE_LOG(LogBF6Script, Warning,
					TEXT("No script project is open, so there is nowhere to put your source."));
				return;
			}
			const FString Dir = GProjectDir;

			// What the bundle says it was built from, so the picker is opened
			// with the answer already in front of them rather than a bare
			// dialog and a guess.
			const FString Bundle = FPaths::Combine(Dir, TEXT("dist"), TEXT("bundle.ts"));
			int32 Modules = 0;
			if (BF6Script::LooksBundled(Bundle, Modules))
			{
				TArray<FString> Names;
				BF6Script::BundledModuleNames(Bundle, Names);
				UE_LOG(LogBF6Script, Display,
					TEXT("This experience was built from %d file(s). Pick the folder that holds them:"),
					Modules);
				for (int32 i = 0; i < Names.Num() && i < 8; i++)
				{
					UE_LOG(LogBF6Script, Display, TEXT("   %s"), *Names[i]);
				}
				if (Names.Num() > 8)
				{
					UE_LOG(LogBF6Script, Display, TEXT("   ... and %d more"), Names.Num() - 8);
				}
			}

			FString Folder = FString::Join(Args, TEXT(" ")).TrimStartAndEnd();
			if (Folder.IsEmpty())
			{
				IDesktopPlatform* DP = FDesktopPlatformModule::Get();
				if (!DP)
				{
					UE_LOG(LogBF6Script, Warning,
						TEXT("BF6.Script.UseMySource <folder>  (no picker available here)"));
					return;
				}
				const void* Parent = FSlateApplication::IsInitialized()
					? FSlateApplication::Get().FindBestParentWindowHandleForDialogs(nullptr)
					: nullptr;
				FString Picked;
				if (!DP->OpenDirectoryDialog(Parent,
					TEXT("Choose the src folder of the project this experience was built from"),
					FPaths::ProjectDir(), Picked))
				{
					return;   // cancelled, which is a decision and not a failure
				}
				Folder = Picked;
			}

			FString Why;
			if (!BF6Script::UseSourceFolder(Dir, Folder, Why))
			{
				UE_LOG(LogBF6Script, Warning, TEXT("BF6.Script.UseMySource: %s"), *Why);
				return;
			}
			UE_LOG(LogBF6Script, Display,
				TEXT("Your source is in. Press BUILD to bundle it the way the site did."));
		})));

	GCmds.Add(IConsoleManager::Get().RegisterConsoleCommand(TEXT("BF6.Script.New"),
		TEXT("BF6.Script.New <name>  Create a script project from the Portal template."),
		FConsoleCommandWithArgsDelegate::CreateLambda([](const TArray<FString>& Args)
		{
			BF6Script::NewProject(FString::Join(Args, TEXT(" ")));
		})));

	GCmds.Add(IConsoleManager::Get().RegisterConsoleCommand(TEXT("BF6.Script.Build"),
		TEXT("Type check the open script project's source and, if it passes, run the bundler on it."),
		FConsoleCommandDelegate::CreateStatic(&BF6Script::Build)));

	GCmds.Add(IConsoleManager::Get().RegisterConsoleCommand(TEXT("BF6.Script.LinkToPage"),
		TEXT("Link the open script project to the experience the Portal panel is showing, so PUSH knows where it is allowed to send. The tool never guesses this."),
		FConsoleCommandDelegate::CreateLambda([]
		{
			FString Why;
			if (LinkProjectToPage(Why))
			{
				UE_LOG(LogBF6Script, Display, TEXT("This project now pushes to experience %s, and to no other."), *GExperienceId);
				SendStatus();
			}
			else
			{
				UE_LOG(LogBF6Script, Warning, TEXT("%s"), *Why);
			}
		})));

	GCmds.Add(IConsoleManager::Get().RegisterConsoleCommand(TEXT("BF6.Script.Session.Simulate"),
		TEXT("BF6.Script.Session.Simulate <lost|restored>  Drive the signed-out banner and the PUSH AGAIN offer without waiting for the site."),
		FConsoleCommandWithArgsDelegate::CreateLambda([](const TArray<FString>& Args)
		{
			BF6Script::SimulateSession(Args.Num() ? Args[0] : FString());
		})));

	GCmds.Add(IConsoleManager::Get().RegisterConsoleCommand(TEXT("BF6.Script.Status"),
		TEXT("Print what the script editor knows: node, template, project, page and Portal panel."),
		FConsoleCommandDelegate::CreateStatic(&BF6Script::LogStatus)));

	GSelTick = FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateStatic(&SelectionTick), 0.5f);

	UE_LOG(LogBF6Script, Display,
		TEXT("BF6 Script ready. Window > Tools > BF6 Script, or BF6.Script.Open."));
}

void BF6Script::Unregister()
{
	for (IConsoleObject* C : GCmds) if (C) IConsoleManager::Get().UnregisterConsoleObject(C);
	GCmds.Reset();

	if (GSelTick.IsValid()) { FTSTicker::GetCoreTicker().RemoveTicker(GSelTick); GSelTick.Reset(); }
	if (GTailTick.IsValid()) { FTSTicker::GetCoreTicker().RemoveTicker(GTailTick); GTailTick.Reset(); }
	if (GChunkTick.IsValid()) { FTSTicker::GetCoreTicker().RemoveTicker(GChunkTick); GChunkTick.Reset(); }
	GTailing = false;
	GChunks.Reset();

	if (GRun.IsValid())
	{
		if (GRun->Tick.IsValid()) FTSTicker::GetCoreTicker().RemoveTicker(GRun->Tick);
		if (GRun->Handle.IsValid())
		{
			FPlatformProcess::TerminateProc(GRun->Handle, true);
			FPlatformProcess::CloseProc(GRun->Handle);
		}
		FPlatformProcess::ClosePipe(GRun->Read, GRun->Write);
		GRun.Reset();
	}

	BF6PortalWeb::RegisterInjectedScript(kSiteScriptId, FString());
	BF6PortalWeb::UnregisterBridgeObject(kBridgeName);

	FGlobalTabmanager::Get()->UnregisterNomadTabSpawner(kTabId);

	GBrowser.Reset();
	if (GWindow.IsValid()) { GWindow->CloseBrowser(true); GWindow.Reset(); }
	GHost.Reset();
	GBridge.Reset();
	GPageReady = false;
}
