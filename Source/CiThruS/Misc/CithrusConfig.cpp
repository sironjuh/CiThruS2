#include "CithrusConfig.h"

#include "Math/UnrealMathUtility.h"

void UCithrusConfig::SetShowIntroduction(bool value)
{
	UCithrusConfig* config = GetMutableDefault<UCithrusConfig>();

	config->ShowIntroduction = value;
	config->SaveConfig();
}

bool UCithrusConfig::GetShowIntroduction()
{
	UCithrusConfig* config = GetMutableDefault<UCithrusConfig>();

	return config->ShowIntroduction;
}

bool UCithrusConfig::LoadViewSynthPreviewLayout(float& outXPercent, float& outYPercent, float& outSizePercent)
{
	const UCithrusConfig* config = GetDefault<UCithrusConfig>();

	if (!config->HasSavedViewSynthPreviewLayout)
	{
		return false;
	}

	outXPercent = FMath::Clamp(config->ViewSynthPreviewXPercent, 0.0f, 100.0f);
	outYPercent = FMath::Clamp(config->ViewSynthPreviewYPercent, 0.0f, 100.0f);
	outSizePercent = FMath::Clamp(config->ViewSynthPreviewSizePercent, 0.0f, 100.0f);

	return true;
}

void UCithrusConfig::SaveViewSynthPreviewLayout(float xPercent, float yPercent, float sizePercent)
{
	UCithrusConfig* config = GetMutableDefault<UCithrusConfig>();

	config->HasSavedViewSynthPreviewLayout = true;
	config->ViewSynthPreviewXPercent = FMath::Clamp(xPercent, 0.0f, 100.0f);
	config->ViewSynthPreviewYPercent = FMath::Clamp(yPercent, 0.0f, 100.0f);
	config->ViewSynthPreviewSizePercent = FMath::Clamp(sizePercent, 0.0f, 100.0f);
	config->SaveConfig();
}
