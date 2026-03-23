#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "Video/HevcEncoderBackend.h"
#include "Widgets/SOverlay.h"

#include <iostream>
#include <memory>
#include <fstream>
#include <thread>
#include <mutex>
#include <vector>

#include "ViewSynthesizer.generated.h"

class USceneCaptureComponent2D;
class UTextureRenderTarget2D;
class UCheckBox;
class USpinBox;
class UUserWidget;
class RenderTargetReader;
class RenderTargetWriter;
class AsyncPipelineRunner;
class SBox;
struct FSlateBrush;

UENUM(BlueprintType)
enum class EHevcDecoderBackend : uint8
{
	Auto UMETA(DisplayName = "Auto"),
	OpenHEVC UMETA(DisplayName = "OpenHEVC"),
	VideoToolbox UMETA(DisplayName = "VideoToolbox")
};

// Performs view synthesis
UCLASS()
class AViewSynthesizer : public AActor
{
	GENERATED_BODY()
	
public:	
	AViewSynthesizer();

	virtual void Tick(float deltaTime) override;

	inline virtual bool ShouldTickIfViewportsOnly() const override { return useEditorTick_; }

	UFUNCTION(BlueprintCallable, CallInEditor, Category = "Stream Controls")
	void StartTransmit();

	UFUNCTION(BlueprintCallable, CallInEditor, Category = "Stream Controls")
	void StopTransmit();

	UFUNCTION(BlueprintCallable, Category = "Preview Layout")
	void SetPreviewXPercent(float value);

	UFUNCTION(BlueprintCallable, Category = "Preview Layout")
	void SetPreviewYPercent(float value);

	UFUNCTION(BlueprintCallable, Category = "Preview Layout")
	void SetPreviewSizePercent(float value);

	UFUNCTION(BlueprintCallable, Category = "Preview Layout")
	float GetPreviewXPercent() const;

	UFUNCTION(BlueprintCallable, Category = "Preview Layout")
	float GetPreviewYPercent() const;

	UFUNCTION(BlueprintCallable, Category = "Preview Layout")
	float GetPreviewSizePercent() const;

	UFUNCTION(BlueprintCallable, Category = "Preview Layout")
	void LoadPreviewLayout();

	UFUNCTION(BlueprintCallable, Category = "Preview Layout")
	void SavePreviewLayout();

	UFUNCTION(BlueprintCallable, Category = "Stream Controls")
	void SetUseLegacyPackedStreamFormat(bool value);

	UFUNCTION(BlueprintCallable, Category = "Stream Controls")
	bool GetUseLegacyPackedStreamFormat() const;

	UPROPERTY(BlueprintReadWrite, EditAnywhere, Category = "General Stream Settings")
	FString remoteStreamIp_ = "127.0.0.1";

	UPROPERTY(BlueprintReadWrite, EditAnywhere, Category = "General Stream Settings")
	int remoteStreamPort_ = 12300;

	UPROPERTY(BlueprintReadWrite, EditAnywhere, Category = "General Stream Settings")
	int remoteStreamWidth_ = 640;

	UPROPERTY(BlueprintReadWrite, EditAnywhere, Category = "General Stream Settings")
	int remoteStreamHeight_ = 480;

	UPROPERTY(BlueprintReadWrite, EditAnywhere, Category = "General Stream Settings")
	float frontFov_ = 90.0f;

	UPROPERTY(BlueprintReadWrite, EditAnywhere, Category = "General Stream Settings")
	float rearFov_ = 60.0f;

	UPROPERTY(BlueprintReadWrite, EditAnywhere, Category = "General Stream Settings")
	bool saveToFile_ = false;

	UPROPERTY(BlueprintReadWrite, EditAnywhere, Category = "General Stream Settings")
	FString saveDirectory_ = FString(FPlatformProcess::UserDir()) + "CiThruS2/Recorded/";

	UPROPERTY(BlueprintReadWrite, EditAnywhere, Category = "General Stream Settings")
	EHevcDecoderBackend hevcDecoderBackend_ = EHevcDecoderBackend::Auto;

	UPROPERTY(BlueprintReadWrite, EditAnywhere, Category = "General Stream Settings")
	EHevcEncoderBackend hevcEncoderBackend_ = EHevcEncoderBackend::Auto;

	UPROPERTY(
		BlueprintReadWrite,
		EditAnywhere,
		Category = "General Stream Settings",
		meta = (
			DisplayName = "Use Legacy Packed Stream Format",
			ToolTip = "When enabled, outgoing view-synthesis streams use the current doubled-height packed format. Disable to send color-only HEVC frames at the configured width and height."))
	bool useLegacyPackedStreamFormat_ = true;

	UPROPERTY(BlueprintReadWrite, EditAnywhere, Category = "General Stream Settings")
	float targetBitrateMbps_ = 2.0f;

	UPROPERTY(BlueprintReadWrite, EditAnywhere, Category = "General Stream Settings")
	int maxKeyFrameInterval_ = 60;

	UPROPERTY(
		BlueprintReadWrite,
		EditAnywhere,
		Category = "General Stream Settings",
		meta = (
			ToolTip = "Total target FPS budget across the outgoing view-synthesis RTP inputs. When both front and rear streams are active, each stream is sent at roughly half of this rate."))
	int maxStreamFps_ = 30;

	UPROPERTY(BlueprintReadWrite, EditAnywhere, Category = "General Stream Settings")
	bool dropFramesWhenBusy_ = true;

	UPROPERTY(BlueprintReadWrite, EditAnywhere, Category = "General Stream Settings")
	int maxReceiveViewFps_ = 30;

	UPROPERTY(BlueprintReadWrite, EditAnywhere, Category = "Kvazaar Settings")
	int overlappedWavefront_ = 3;

	UPROPERTY(BlueprintReadWrite, EditAnywhere, Category = "Kvazaar Settings")
	int wavefrontParallelProcessing_ = 1;

	UPROPERTY(BlueprintReadWrite, EditAnywhere, Category = "Kvazaar Settings")
	int quantizationParameter_ = 27;

	UPROPERTY(BlueprintReadWrite, EditAnywhere, Category = "Depth Settings")
	float depthRange_ = 150.0f;

	UPROPERTY(BlueprintReadWrite, EditAnywhere, Category = "Depth Settings")
	TObjectPtr<UTextureRenderTarget2D> resultRenderTarget_ = nullptr;

	UPROPERTY(BlueprintReadWrite, EditAnywhere, Category = "Preview Layout", meta = (ClampMin = "0.0", ClampMax = "100.0"))
	float previewXPercent_ = 0.0f;

	UPROPERTY(BlueprintReadWrite, EditAnywhere, Category = "Preview Layout", meta = (ClampMin = "0.0", ClampMax = "100.0"))
	float previewYPercent_ = 0.0f;

	UPROPERTY(BlueprintReadWrite, EditAnywhere, Category = "Preview Layout", meta = (ClampMin = "0.0", ClampMax = "100.0"))
	float previewSizePercent_ = 0.0f;

protected:
	struct ViewSynthCameraParams
	{
		uint32_t frameNumber;
		FVector3f pos;
		FVector3f rot;
		float fov;
		float depthRange;
		uint64_t timestamp;
	};

	bool transmitEnabled_ = false;
	bool useEditorTick_ = true;

	std::mutex streamMutex_;

	USceneCaptureComponent2D* frontCamera_ = nullptr;
	USceneCaptureComponent2D* rearCamera_ = nullptr;

	UPROPERTY()
	TObjectPtr<UTextureRenderTarget2D> frontRenderTarget_ = nullptr;
	UPROPERTY()
	TObjectPtr<UTextureRenderTarget2D> rearRenderTarget_ = nullptr;

	RenderTargetReader* frontReader_;
	RenderTargetReader* rearReader_;

	RenderTargetWriter* resultWriter_;

	std::vector<AsyncPipelineRunner*> runners_;

	uint32_t frameNumber_;
	uint64_t startTimestampMs_;
	double captureAccumulator_ = 0.0;
	double previewLayoutLastUiChangeTime_ = 0.0;

	bool wantsStop_ = false;
	bool previewLayoutLoaded_ = false;
	bool previewLayoutControlsSyncInProgress_ = false;
	bool previewLayoutSavePending_ = false;
	bool mainMenuWidgetClassLookupAttempted_ = false;
	bool mainMenuWidgetObserved_ = false;
	bool viewSynthesisControlWidgetClassLookupAttempted_ = false;

	TWeakObjectPtr<UUserWidget> mainMenuWidget_;
	TWeakObjectPtr<UUserWidget> viewSynthesisControlWidget_;
	TWeakObjectPtr<UCheckBox> legacyPackedStreamFormatCheckBox_;
	TWeakObjectPtr<USpinBox> previewXSpinBox_;
	TWeakObjectPtr<USpinBox> previewYSpinBox_;
	TWeakObjectPtr<USpinBox> previewSizeSpinBox_;
	UClass* mainMenuWidgetClass_ = nullptr;
	UClass* viewSynthesisControlWidgetClass_ = nullptr;
	TSharedPtr<SOverlay> previewOverlayWidget_;
	TSharedPtr<SBox> previewBox_;
	TSharedPtr<FSlateBrush> previewBrush_;
	SOverlay::FOverlaySlot* previewOverlaySlot_ = nullptr;

	virtual void BeginPlay() override;
	virtual void PostRegisterAllComponents() override;
	virtual void EndPlay(const EEndPlayReason::Type endPlayReason) override;

	bool StartStreams();
	void DeleteStreams();
	bool ResetStreams();

	void CreatePreviewOverlay();
	void DestroyPreviewOverlay();
	void UpdatePreviewOverlay();
	void UpdatePreviewOverlayLayout();
	void UpdatePreviewLayoutControls();
	void TryInstallPreviewLayoutControls();
	UUserWidget* FindViewSynthesisControlWidget();
	AViewSynthesizer* ResolveControlledSynthesizer(UUserWidget* widget) const;
	void ResetPreviewLayoutWidgetState();
	float ClampPreviewPercent(float value) const;
	void ApplyPreviewLayout(float xPercent, float yPercent, float sizePercent);
	void ApplyLegacyPreviewLayoutDefaults();
	void QueuePreviewLayoutSave();
	void FlushPendingPreviewLayoutSave();
	bool TryGetPreviewViewportSize(FVector2D& outViewportSize) const;
	uint32 GetOutgoingViewStreamCount() const;
	double GetEffectiveCaptureFps() const;
	uint32 GetExpectedPerStreamFps() const;

	UFUNCTION()
	void HandlePreviewXSpinBoxValueChanged(float value);

	UFUNCTION()
	void HandlePreviewYSpinBoxValueChanged(float value);

	UFUNCTION()
	void HandlePreviewSizeSpinBoxValueChanged(float value);

	UFUNCTION()
	void HandleUseLegacyPackedStreamFormatCheckStateChanged(bool bIsChecked);

	UUserWidget* FindMainMenuWidget();
	bool IsMainMenuOpen();

	void StopTransmitInternal();
	void Capture();
};
