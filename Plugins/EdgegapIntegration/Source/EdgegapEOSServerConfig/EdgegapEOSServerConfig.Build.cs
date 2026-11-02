// Copyright Edgegap. All Rights Reserved.

using UnrealBuildTool;

public class EdgegapEOSServerConfig : ModuleRules
{
	public EdgegapEOSServerConfig(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;

		// Loads at EarliestPossible (after config, before the online subsystem and UObjects): Core only.
		PrivateDependencyModuleNames.Add("Core");
	}
}
