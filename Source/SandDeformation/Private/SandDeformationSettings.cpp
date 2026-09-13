// Copyright Matin. All Rights Reserved.

#include "SandDeformationSettings.h"

USandDeformationSettings::USandDeformationSettings()
	: TextureResolution(1024, 1024)
	, RegionSizeWorld(4096.0f)
	, AngleOfReposeDegrees(34.0f)
	, SlumpRate(6.0f)
	, HeightRestoreRate(0.0f)
	, RippleSpeed(120.0f)
	, RippleDamping(1.5f)
	, DisturbanceDecay(0.8f)
	, NormalStrength(1.0f)
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
