// Copyright Matin. All Rights Reserved.

#include "SandDeformationSettings.h"

USandDeformationSettings::USandDeformationSettings()
	: TextureResolution(1024, 1024)
	, RegionSizeWorld(4096.0f)
	, AngleOfReposeDegrees(34.0f)
	, SlumpRate(6.0f)
	, HeightRestoreRate(0.0f)
	, RippleSpeed(450.0f)
	, RippleDamping(0.6f)
	, DisturbanceDecay(0.35f)
	, RippleSubSteps(4)
	, NormalStrength(3.5f)
	, HeightScale(1.0f)
	, RegionParameterName(TEXT("SandRegion"))
	, SandDataParameterName(TEXT("SandData"))
{
}

FName USandDeformationSettings::GetCategoryName() const
{
	return TEXT("Plugins");
}

const USandDeformationSettings& USandDeformationSettings::Get()
{
	const USandDeformationSettings* Settings = GetDefault<USandDeformationSettings>();
	check(Settings);
	return *Settings;
}
