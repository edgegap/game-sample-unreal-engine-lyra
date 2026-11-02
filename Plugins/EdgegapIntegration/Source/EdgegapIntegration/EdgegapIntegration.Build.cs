// Copyright Edgegap. All Rights Reserved.

using UnrealBuildTool;

public class EdgegapIntegration : ModuleRules
{
	public EdgegapIntegration(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;

		PublicDependencyModuleNames.AddRange(
			new string[]
			{
				"Core",
				"CoreUObject",
				"Engine",
				"CommonUser",
			});

		PrivateDependencyModuleNames.AddRange(
			new string[]
			{
				"HTTP",
				"Json",
				"Sockets",
				"Slate",
				"SlateCore",
				"OnlineSubsystem",
				"OnlineSubsystemUtils",
			});
	}
}
