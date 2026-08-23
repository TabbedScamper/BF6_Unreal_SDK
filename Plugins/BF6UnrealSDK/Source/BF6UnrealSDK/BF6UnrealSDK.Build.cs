using System.IO;
using UnrealBuildTool;

public class BF6UnrealSDK : ModuleRules
{
	public BF6UnrealSDK(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = ModuleRules.PCHUsageMode.UseExplicitOrSharedPCHs;

		PublicDependencyModuleNames.AddRange(new string[]
		{
			"Core",
			"CoreUObject",
			"Engine",
			"Projects",                 // IPluginManager, to find the plugin's folder
			"ProceduralMeshComponent",  // build the decoded geometry into a mesh
			"Json",                     // save/load session files
			"JsonUtilities"
		});

		PrivateDependencyModuleNames.AddRange(new string[]
		{
			"Slate",
			"SlateCore",
			"InputCore",
			"WorkspaceMenuStructure",   // the Window > Tools menu group
			"ImageWrapper",             // decode the map thumbnail PNGs for Slate
			"HTTP"                      // update check + download from GitHub releases
		});

		if (Target.bBuildEditor)
		{
			PrivateDependencyModuleNames.AddRange(new string[]
			{
				"UnrealEd",               // GEditor world
				"MaterialEditor",         // the runtime-built volume colour material
				"AppFramework",           // SColorPicker: the hue wheel, hex field and alpha
				"ToolMenus",              // the actor context menu, in the tree and the viewport
				"AdvancedPreviewScene",   // lit preview scene for the 3D model viewport
				"MainFrame",              // auto-open the map picker once the editor is ready
				"DesktopPlatform",        // native open-file dialog for importing .spatial.json
				"LevelEditor",             // viewport overlay (Build Mode chrome) + active viewport
				"SceneOutliner"        // the SDK-styled scene tree
			});
		}

		// The engine-neutral decode core. We only need its C header to compile
		// against; the .dll itself is loaded at runtime, so no static linking and
		// no toolchain-version fight with whatever built the core.
		string ThirdParty = Path.Combine(ModuleDirectory, "..", "ThirdParty", "libbf6");
		PublicIncludePaths.Add(Path.Combine(ThirdParty, "include"));

		// Stage bf6_core.dll next to the plugin's binaries so GetDllHandle finds
		// it after a packaged build too.
		string DllName = "bf6_core.dll";
		string DllSrc = Path.Combine(ThirdParty, "bin", "Win64", DllName);
		RuntimeDependencies.Add(Path.Combine("$(BinaryOutputDir)", DllName), DllSrc);

		// The SDK's placeable catalogue (level_info.json + asset_types.json). The
		// module reads these at runtime from the plugin folder to build the
		// per-map object list, so they must ride along with a packaged build.
		string FbData = Path.Combine(ThirdParty, "data", "FbExportData");
		RuntimeDependencies.Add(Path.Combine(FbData, "level_info.json"));
		RuntimeDependencies.Add(Path.Combine(FbData, "asset_types.json"));

		// The map thumbnail PNGs shown in the browser's card grid.
		string MapDir = Path.Combine(ThirdParty, "data", "maps");
		if (Directory.Exists(MapDir))
			foreach (string png in Directory.GetFiles(MapDir, "*.png"))
				RuntimeDependencies.Add(png);
	}
}
