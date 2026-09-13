// Copyright Matin. All Rights Reserved.

using UnrealBuildTool;

public class SandDeformation : ModuleRules
{
	public SandDeformation(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = ModuleRules.PCHUsageMode.UseExplicitOrSharedPCHs;

		PublicDependencyModuleNames.AddRange(
			new string[]
			{
				"Core",
				"CoreUObject",
				"Engine",
				// USandDeformationSettings derives from UDeveloperSettings, so
				// anything including our public headers needs this too.
				"DeveloperSettings",
			}
		);

		PrivateDependencyModuleNames.AddRange(
			new string[]
			{
				"Projects",
				"RHI",
				"RenderCore",
				// Impact bursts. Private on purpose: no public header exposes a
				// Niagara type, so a consumer never has to care that we use it.
				"Niagara",
			}
		);
	}
}
