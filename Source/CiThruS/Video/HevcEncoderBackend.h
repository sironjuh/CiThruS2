#pragma once

#include "CoreMinimal.h"
#include "HevcEncoderBackend.generated.h"

UENUM(BlueprintType)
enum class EHevcEncoderBackend : uint8
{
	Auto UMETA(DisplayName = "Auto"),
	VideoToolbox UMETA(DisplayName = "VideoToolbox"),
	Kvazaar UMETA(DisplayName = "Kvazaar")
};
