// Copyright Tanushk Nirmal 2026 All Rights Reserved.

using UnrealBuildTool;
using System.IO;

public class DynamicPayloadSystem : ModuleRules
{
	public DynamicPayloadSystem(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;

		// Strict include-what-you-use (replaces the old bEnforceIWYU flag).
		IWYUSupport = IWYUSupport.Full;

		PublicIncludePaths.AddRange(new string[]
		{
			Path.Combine(ModuleDirectory, "Public")
		});

		PrivateIncludePaths.AddRange(new string[]
		{
			Path.Combine(ModuleDirectory, "Private")
		});

		PublicDependencyModuleNames.AddRange(new string[]
		{
			"Core",
			"CoreUObject",
			"Engine",
			"Niagara",
			"PhysicsCore"
		});

		PrivateDependencyModuleNames.AddRange(new string[]
		{
			"InputCore"
		});

		// Platform-specific configuration
		if (Target.Platform == UnrealTargetPlatform.Win64)
		{
			// Windows-specific settings (none required currently)
		}
		else if (Target.Platform == UnrealTargetPlatform.Linux ||
				 Target.Platform == UnrealTargetPlatform.Mac)
		{
			// Unix-specific settings (none required currently)
		}
	}
}
