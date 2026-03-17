#include "CoreMinimal.h"

#include "CithrusConfig.generated.h"

// Used to save and load persistent configuration values in Saved/Config/.../Game.ini
UCLASS( Config=Game )
class CITHRUS_API UCithrusConfig : public UObject
{
	GENERATED_BODY()

public:
	UFUNCTION(BlueprintCallable)
	static void SetShowIntroduction(bool value);

	UFUNCTION(BlueprintCallable)
	static bool GetShowIntroduction();

	static bool LoadViewSynthPreviewLayout(float& outXPercent, float& outYPercent, float& outSizePercent);
	static void SaveViewSynthPreviewLayout(float xPercent, float yPercent, float sizePercent);

private:
	UPROPERTY(Config)
	bool ShowIntroduction = true;

	UPROPERTY(Config)
	bool HasSavedViewSynthPreviewLayout = false;

	UPROPERTY(Config)
	float ViewSynthPreviewXPercent = 0.0f;

	UPROPERTY(Config)
	float ViewSynthPreviewYPercent = 0.0f;

	UPROPERTY(Config)
	float ViewSynthPreviewSizePercent = 0.0f;
};
