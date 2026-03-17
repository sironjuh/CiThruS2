#include "ViewSynthesizer.h"
#include "Video/Pipeline/RenderTargetReader.h"
#include "Video/Pipeline/RenderTargetWriter.h"
#include "Video/Pipeline/FloatToByteConverter.h"
#include "Video/Pipeline/RgbaToYuvConverter.h"
#include "Video/Pipeline/YuvToRgbaConverter.h"
#include "Video/Pipeline/DepthToYuvConverter.h"
#include "Video/Pipeline/DepthSeparator.h"
#include "Video/Pipeline/ImageConcatenator.h"
#include "Video/Pipeline/SeiEmbedder.h"
#include "Video/Pipeline/CsvLogger.h"
#include "Video/Pipeline/HevcEncoder.h"
#include "Video/Pipeline/HevcDecoder.h"
#include "Video/Pipeline/FrameRateLimiter.h"
#include "Video/Pipeline/Pipeline.h"
#include "Video/Pipeline/SolidColorImageGenerator.h"
#include "Video/Pipeline/RtpTransmitter.h"
#include "Video/Pipeline/RtpReceiver.h"
#include "Video/Pipeline/BlinkerSource.h"
#include "Video/Pipeline/BlinkDetector.h"
#include "Video/Pipeline/PngRecorder.h"
#include "Video/Pipeline/FileSink.h"
#include "Video/Pipeline/ScaffoldingAdapter.h"
#include "Video/Pipeline/ScaffoldingDuplicator.h"
#include "Video/Pipeline/ScaffoldingSidechainSource.h"
#include "Video/Pipeline/ScaffoldingSequentialFilter.h"
#include "Video/Pipeline/ScaffoldingSequentialSink.h"
#include "Video/Pipeline/ScaffoldingParallelFilter.h"
#include "Video/Pipeline/ScaffoldingParallelSource.h"
#include "Video/Pipeline/ScaffoldingParallelSink.h"
#include "Video/Pipeline/AsyncPipelineRunner.h"
#include "Misc/CithrusConfig.h"
#include "Misc/Debug.h"

#include "RHI.h"
#include "Blueprint/UserWidget.h"
#include "Blueprint/WidgetTree.h"
#include "Blueprint/WidgetBlueprintLibrary.h"
#include "Components/HorizontalBox.h"
#include "Components/HorizontalBoxSlot.h"
#include "Components/PanelWidget.h"
#include "Engine/Engine.h"
#include "Engine/GameViewportClient.h"
#include "Engine/TextureRenderTarget2D.h"
#include "Components/SceneCaptureComponent2D.h"
#include "Components/SizeBox.h"
#include "Components/SpinBox.h"
#include "Components/TextBlock.h"
#include "Components/VerticalBox.h"
#include "Components/VerticalBoxSlot.h"
#include "ShaderParameterStruct.h"
#include "DataDrivenShaderPlatformInfo.h"
#include "RenderResource.h"
#include "Styling/SlateBrush.h"
#include "UObject/UnrealType.h"
#include "Widgets/Images/SImage.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/SOverlay.h"

#include <algorithm>

namespace
{
constexpr TCHAR MAIN_MENU_WIDGET_CLASS_PATH[] = TEXT("/Game/UI/WBP_MainMenu.WBP_MainMenu_C");
constexpr TCHAR VIEW_SYNTHESIS_CONTROL_WIDGET_CLASS_PATH[] = TEXT("/Game/ViewSynthesis/ViewSynthesisControlWidget.ViewSynthesisControlWidget_C");
constexpr TCHAR CONTROLLED_SYNTHESIZER_PROPERTY_NAME[] = TEXT("ControlledSynthesizer");
constexpr TCHAR PREVIEW_LAYOUT_SECTION_WIDGET_NAME[] = TEXT("PreviewLayoutSection");
constexpr TCHAR PREVIEW_X_SPIN_BOX_WIDGET_NAME[] = TEXT("PreviewLayoutXSpinBox");
constexpr TCHAR PREVIEW_Y_SPIN_BOX_WIDGET_NAME[] = TEXT("PreviewLayoutYSpinBox");
constexpr TCHAR PREVIEW_SIZE_SPIN_BOX_WIDGET_NAME[] = TEXT("PreviewLayoutSizeSpinBox");
constexpr float LEGACY_PREVIEW_MARGIN = 16.0f;
constexpr float LEGACY_PREVIEW_WIDTH = 320.0f;
constexpr float PREVIEW_PERCENT_MIN = 0.0f;
constexpr float PREVIEW_PERCENT_MAX = 100.0f;
constexpr float PREVIEW_SPIN_BOX_DELTA = 0.1f;
constexpr float PREVIEW_LAYOUT_LABEL_WIDTH = 180.0f;
constexpr float PREVIEW_LAYOUT_ROW_PADDING = 4.0f;
constexpr float PREVIEW_LAYOUT_SECTION_PADDING = 8.0f;
constexpr double PREVIEW_LAYOUT_SAVE_DEBOUNCE_SECONDS = 0.25;
constexpr int32 PREVIEW_VIEWPORT_Z_ORDER = 100;
constexpr float LEGACY_FALLBACK_VIEWPORT_WIDTH = 1280.0f;
constexpr float LEGACY_FALLBACK_VIEWPORT_HEIGHT = 720.0f;
constexpr float MAIN_MENU_GRACE_PERIOD_SECONDS = 1.0f;
}

AViewSynthesizer::AViewSynthesizer()
{
    RootComponent = CreateDefaultSubobject<USceneComponent>(TEXT("RootComponent"));

    frontCamera_ = CreateDefaultSubobject<USceneCaptureComponent2D>(TEXT("FrontCamera"));
    frontCamera_->SetupAttachment(RootComponent);
    frontCamera_->SetRelativeLocation(FVector(200.0f, 0.0f, 0.0f));
    frontCamera_->CaptureSource = ESceneCaptureSource::SCS_SceneColorSceneDepth;

    rearCamera_ = CreateDefaultSubobject<USceneCaptureComponent2D>(TEXT("RearCamera"));
    rearCamera_->SetupAttachment(RootComponent);
    rearCamera_->CaptureSource = ESceneCaptureSource::SCS_SceneColorSceneDepth;

    // Set this actor to call Tick() every frame
    PrimaryActorTick.bCanEverTick = true;
    //PrimaryActorTick.TickGroup = TG_PostUpdateWork;
}

void AViewSynthesizer::BeginPlay()
{
	Super::BeginPlay();

	LoadPreviewLayout();
	CreatePreviewOverlay();
}

void AViewSynthesizer::PostRegisterAllComponents()
{
    Super::PostRegisterAllComponents();

    static const uint16_t RENDER_TARGET_DEFAULT_RESOLUTION = 512;

    frontRenderTarget_ = NewObject<UTextureRenderTarget2D>();
    frontRenderTarget_->InitCustomFormat(RENDER_TARGET_DEFAULT_RESOLUTION, RENDER_TARGET_DEFAULT_RESOLUTION, PF_A32B32G32R32F, false);
    frontRenderTarget_->RenderTargetFormat = RTF_RGBA32f;

    frontCamera_->TextureTarget = frontRenderTarget_;
    frontCamera_->bCaptureEveryFrame = false;
    frontCamera_->bCaptureOnMovement = false;
    //frontCamera_->ShowFlags.DynamicShadows = false;

    rearRenderTarget_ = NewObject<UTextureRenderTarget2D>();
    rearRenderTarget_->InitCustomFormat(RENDER_TARGET_DEFAULT_RESOLUTION, RENDER_TARGET_DEFAULT_RESOLUTION, PF_A32B32G32R32F, false);
    rearRenderTarget_->RenderTargetFormat = RTF_RGBA32f;

    rearCamera_->TextureTarget = rearRenderTarget_;
    rearCamera_->bCaptureEveryFrame = false;
    rearCamera_->bCaptureOnMovement = false;
    //rearCamera_->ShowFlags.DynamicShadows = false;
}

void AViewSynthesizer::EndPlay(const EEndPlayReason::Type endPlayReason)
{
	FlushPendingPreviewLayoutSave();
	DestroyPreviewOverlay();
	ResetPreviewLayoutWidgetState();

	Super::EndPlay(endPlayReason);

	DeleteStreams();
}

void AViewSynthesizer::Tick(float deltaTime)
{
	Super::Tick(deltaTime);

	UpdatePreviewOverlay();
	TryInstallPreviewLayoutControls();
	FlushPendingPreviewLayoutSave();

	if (wantsStop_)
	{
		StopTransmitInternal();
	}

	const std::lock_guard<std::mutex> lock(streamMutex_);

	if (!transmitEnabled_)
	{
		return;
	}

	if (!saveToFile_ && maxStreamFps_ > 0)
	{
		const double frameInterval = 1.0 / static_cast<double>(maxStreamFps_);
		captureAccumulator_ += static_cast<double>(deltaTime);
		if (captureAccumulator_ < frameInterval)
		{
			return;
		}
		captureAccumulator_ = std::min(captureAccumulator_ - frameInterval, frameInterval);
	}

	Capture();
}

void AViewSynthesizer::StartTransmit()
{
    const std::lock_guard<std::mutex> lock(streamMutex_);

    if (ResetStreams())
    {
        captureAccumulator_ = 0.0;
        transmitEnabled_ = true;
        useEditorTick_ = true;
    }
}

void AViewSynthesizer::StopTransmit()
{
	// Stop the transmit in a synchronized manner to avoid race conditions
	wantsStop_ = true;
}

void AViewSynthesizer::SetPreviewXPercent(float value)
{
	previewXPercent_ = ClampPreviewPercent(value);
	UpdatePreviewOverlayLayout();
	UpdatePreviewLayoutControls();
}

void AViewSynthesizer::SetPreviewYPercent(float value)
{
	previewYPercent_ = ClampPreviewPercent(value);
	UpdatePreviewOverlayLayout();
	UpdatePreviewLayoutControls();
}

void AViewSynthesizer::SetPreviewSizePercent(float value)
{
	previewSizePercent_ = ClampPreviewPercent(value);
	UpdatePreviewOverlayLayout();
	UpdatePreviewLayoutControls();
}

float AViewSynthesizer::GetPreviewXPercent() const
{
	return previewXPercent_;
}

float AViewSynthesizer::GetPreviewYPercent() const
{
	return previewYPercent_;
}

float AViewSynthesizer::GetPreviewSizePercent() const
{
	return previewSizePercent_;
}

void AViewSynthesizer::LoadPreviewLayout()
{
	float loadedXPercent = 0.0f;
	float loadedYPercent = 0.0f;
	float loadedSizePercent = 0.0f;

	if (UCithrusConfig::LoadViewSynthPreviewLayout(loadedXPercent, loadedYPercent, loadedSizePercent))
	{
		ApplyPreviewLayout(loadedXPercent, loadedYPercent, loadedSizePercent);
	}
	else
	{
		ApplyLegacyPreviewLayoutDefaults();
	}

	previewLayoutLoaded_ = true;
}

void AViewSynthesizer::SavePreviewLayout()
{
	UCithrusConfig::SaveViewSynthPreviewLayout(previewXPercent_, previewYPercent_, previewSizePercent_);
	previewLayoutSavePending_ = false;
}

bool AViewSynthesizer::StartStreams()
{
    // TODO: More sanity checks should be added here
    if (saveDirectory_.IsEmpty() || saveDirectory_.Contains("\\") || saveDirectory_[saveDirectory_.Len() - 1] != '/')
    {
        Debug::Log("Invalid directory");

        return false;
    }

    if (!resultRenderTarget_)
    {
        Debug::Log("Result render target not set");

        return false;
    }

    // Capturing below 16x16 causes corrupted video, might be because of SSE instructions in YUV conversion
    uint16_t frameWidth = std::max(remoteStreamWidth_, 16);
    uint16_t frameHeight = std::max(remoteStreamHeight_, 16);

    // Width and height must be divisible by eight (HEVC limitation)
    // This rounds up to the nearest integers divisible by eight
    frameWidth += (8 - (frameWidth % 8)) % 8;
    frameHeight += (8 - (frameHeight % 8)) % 8;

    const uint32_t expectedStreamFps = static_cast<uint32_t>(std::max(maxStreamFps_, 1));

    wantsStop_ = false;
    frameNumber_ = 0;
    startTimestampMs_ = 0;
    captureAccumulator_ = 0.0;

    frontCamera_->FOVAngle = frontFov_;
    rearCamera_->FOVAngle = rearFov_;

    frontRenderTarget_->ResizeTarget(frameWidth, frameHeight);
    rearRenderTarget_->ResizeTarget(frameWidth, frameHeight);
    resultRenderTarget_->ResizeTarget(frameWidth, frameHeight);
    UpdatePreviewOverlayLayout();

    try
    {
        if (saveToFile_)
        {
            frontReader_ = nullptr;
            rearReader_ = new RenderTargetReader({ rearRenderTarget_ }, true, depthRange_);

            runners_.push_back(
                new AsyncPipelineRunner(
                    new Pipeline(
                        rearReader_,
                        new ScaffoldingParallelSink<2>(
                            {
                                new ImageSequentialSink(
                                    {
                                        new DepthSeparator()
                                    },
                                    new PngRecorder(TCHAR_TO_UTF8(*saveDirectory_), frameWidth, frameHeight * 2)),
                                new ImageSequentialSink(
                                    {
                                        new CsvLogger()
                                    },
                                    new FileSink(TCHAR_TO_UTF8(*(saveDirectory_ + FString("log.csv")))))
                            }))));
        }
        else
        {
            frontReader_ = new RenderTargetReader({ frontRenderTarget_ }, true, depthRange_);
            rearReader_ = new RenderTargetReader({ rearRenderTarget_ }, true, depthRange_);

            runners_.push_back(
                new AsyncPipelineRunner(
                    new Pipeline(
                        frontReader_,
                        new ScaffoldingAdapter<2, 1>(
                            new ScaffoldingParallelFilter<2>(
                                {
                                    new ImageSequentialFilter(
                                    {
                                        new ScaffoldingAdapter<1, 1>(
                                            new ScaffoldingDuplicator<2>(),
                                            new ScaffoldingAdapter<2, 1>(
                                                new ScaffoldingParallelFilter<2>(
                                                {
                                                    new RgbaToYuvConverter(frameWidth, frameHeight),
                                                    new DepthToYuvConverter()
                                                }),
                                                new ImageConcatenator<2>(frameWidth, frameHeight))
                                        ),
                                        new HevcEncoder(frameWidth, frameHeight * 2, 16, quantizationParameter_, wavefrontParallelProcessing_, overlappedWavefront_, HevcPresetMinimumLatency, hevcEncoderBackend_, targetBitrateMbps_, static_cast<uint32_t>(maxKeyFrameInterval_), expectedStreamFps)
                                    }),
                                    new ImageSequentialFilter()
                                }),
                            new SeiEmbedder("CiThruSViewSynth")),
                        new RtpTransmitter(TCHAR_TO_UTF8(*remoteStreamIp_), remoteStreamPort_ + 1))));

            runners_.push_back(
                new AsyncPipelineRunner(
                    new Pipeline(
                        rearReader_,
                        new ScaffoldingAdapter<2, 1>(
                            new ScaffoldingParallelFilter<2>(
                                {
                                    new ImageSequentialFilter(
                                        {
                                            new RgbaToYuvConverter(frameWidth, frameHeight),
                                            new ScaffoldingSidechainSource<1, 1>(
                                                new SolidColorImageGenerator(frameWidth, frameHeight, 0, 128, 128),
                                                new ImageConcatenator<2>(frameWidth, frameHeight)),
                                            new HevcEncoder(frameWidth, frameHeight * 2, 16, quantizationParameter_, wavefrontParallelProcessing_, overlappedWavefront_, HevcPresetMinimumLatency, hevcEncoderBackend_, targetBitrateMbps_, static_cast<uint32_t>(maxKeyFrameInterval_), expectedStreamFps),
                                        }),
                                    new ImageSequentialFilter()
                                }),
                            new SeiEmbedder("CiThruSViewSynth")),
                        new RtpTransmitter(TCHAR_TO_UTF8(*remoteStreamIp_), remoteStreamPort_))));

            runners_.push_back(
                new AsyncPipelineRunner(
                    new Pipeline(
                        new RtpReceiver(TCHAR_TO_UTF8(*remoteStreamIp_), remoteStreamPort_ - 1),
                        new ImageSequentialFilter(
                            {
                                new HevcDecoder(16, hevcDecoderBackend_),
                                new FrameRateLimiter(static_cast<uint32_t>(std::max(maxReceiveViewFps_, 0))),
                                new YuvToRgbaConverter(frameWidth, frameHeight, "bgra"),
                                //new BlinkDetector("stop.txt"),
                            }),
                            new RenderTargetWriter(resultRenderTarget_))));
        }
    }
    catch (const std::exception& exception)
    {
        // This leaks memory if some pipeline components are constructed before
        // the exception is thrown. It should be fine since this only happens
        // when the user provides invalid parameters to the pipeline. It's
        // probably not possible to avoid the leak without initializing every
        // pipeline component manually one at a time, and that would get messy

        Debug::Log("Pipeline construction failed: " + std::string(exception.what()));

        DeleteStreams();

        return false;
    }

    return true;
}

void AViewSynthesizer::DeleteStreams()
{
    for (AsyncPipelineRunner* runner : runners_)
    {
        delete runner;
    }

    runners_.clear();
    captureAccumulator_ = 0.0;

    // These are already deleted by the pipeline so don't delete them twice
    frontReader_ = nullptr;
    rearReader_ = nullptr;
}

bool AViewSynthesizer::ResetStreams()
{
    DeleteStreams();

    return StartStreams();
}

void AViewSynthesizer::StopTransmitInternal()
{
    const std::lock_guard<std::mutex> lock(streamMutex_);

    DeleteStreams();

    transmitEnabled_ = false;
    useEditorTick_ = false;
    wantsStop_ = false;
    captureAccumulator_ = 0.0;
}

void AViewSynthesizer::Capture()
{
    if (dropFramesWhenBusy_)
    {
        const bool frontBusy = frontReader_ && frontReader_->IsBusy();
        const bool rearBusy = rearReader_ && rearReader_->IsBusy();
        if (frontBusy || rearBusy)
        {
            return;
        }
    }

    if (frontReader_)
    {
        frontCamera_->CaptureScene();
    }

    if (rearReader_)
    {
        rearCamera_->CaptureScene();
    }

    uint64_t timestampMs = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();

    if (startTimestampMs_ == 0)
    {
        startTimestampMs_ = timestampMs;
    }

    if (frontReader_)
    {
        FTransform frontTfm = FTransform(frontCamera_->GetComponentRotation(), frontCamera_->GetComponentLocation());

        ViewSynthCameraParams frontCameraParams;

        frontCameraParams.frameNumber = frameNumber_;

        frontCameraParams.pos = FVector3f(frontTfm.GetTranslation() / 100.0f);    // cm in Unreal, m in ViewSynth
        frontCameraParams.rot = FVector3f(frontTfm.GetRotation().Euler());

        frontCameraParams.pos = FVector3f(frontCameraParams.pos.Y, frontCameraParams.pos.Z, frontCameraParams.pos.X);
        frontCameraParams.rot = FVector3f(frontCameraParams.rot.Y, -frontCameraParams.rot.Z, frontCameraParams.rot.X);

        frontCameraParams.fov = frontCamera_->FOVAngle;
        frontCameraParams.depthRange = depthRange_;

        frontCameraParams.timestamp = timestampMs - startTimestampMs_;

        const bool frontQueued = dropFramesWhenBusy_
            ? frontReader_->TryRead(reinterpret_cast<uint8_t*>(&frontCameraParams), sizeof(ViewSynthCameraParams))
            : (frontReader_->Read(reinterpret_cast<uint8_t*>(&frontCameraParams), sizeof(ViewSynthCameraParams)), true);
        if (!frontQueued)
        {
            return;
        }
    }

    if (rearReader_)
    {
        FTransform rearTfm = FTransform(rearCamera_->GetComponentRotation(), rearCamera_->GetComponentLocation());

        ViewSynthCameraParams rearCameraParams;

        rearCameraParams.frameNumber = frameNumber_;

        rearCameraParams.pos = FVector3f(rearTfm.GetTranslation() / 100.0f);  // cm in Unreal, m in ViewSynth
        rearCameraParams.rot = FVector3f(rearTfm.GetRotation().Euler());

        rearCameraParams.pos = FVector3f(rearCameraParams.pos.Y, rearCameraParams.pos.Z, rearCameraParams.pos.X);
        rearCameraParams.rot = FVector3f(rearCameraParams.rot.Y, -rearCameraParams.rot.Z, rearCameraParams.rot.X);

        rearCameraParams.fov = rearCamera_->FOVAngle;
        rearCameraParams.depthRange = depthRange_;

        rearCameraParams.timestamp = timestampMs - startTimestampMs_;

        const bool rearQueued = dropFramesWhenBusy_
            ? rearReader_->TryRead(reinterpret_cast<uint8_t*>(&rearCameraParams), sizeof(ViewSynthCameraParams))
            : (rearReader_->Read(reinterpret_cast<uint8_t*>(&rearCameraParams), sizeof(ViewSynthCameraParams)), true);
        if (!rearQueued)
        {
            return;
        }
    }

    frameNumber_++;
}

void AViewSynthesizer::CreatePreviewOverlay()
{
	if (previewOverlayWidget_.IsValid() || !resultRenderTarget_ || !GetWorld() || !GetWorld()->IsGameWorld() || !GEngine || !GEngine->GameViewport)
	{
		return;
	}

	if (!previewLayoutLoaded_)
	{
		LoadPreviewLayout();
	}

	previewBrush_ = MakeShared<FSlateBrush>();
	previewBrush_->SetResourceObject(resultRenderTarget_);
	previewBrush_->DrawAs = ESlateBrushDrawType::Image;
	previewBrush_->TintColor = FSlateColor(FLinearColor::White);

	GEngine->GameViewport->AddViewportWidgetContent(
		SAssignNew(previewOverlayWidget_, SOverlay)
		+ SOverlay::Slot()
		.Expose(previewOverlaySlot_)
		.HAlign(HAlign_Left)
		.VAlign(VAlign_Top)
		.Padding(FMargin(0.0f))
		[
			SAssignNew(previewBox_, SBox)
			[
				SNew(SImage)
				.Image(previewBrush_.Get())
			]
		],
		PREVIEW_VIEWPORT_Z_ORDER);

	UpdatePreviewOverlayLayout();

	if (previewOverlayWidget_.IsValid())
	{
		previewOverlayWidget_->SetVisibility(EVisibility::HitTestInvisible);
	}
}

void AViewSynthesizer::DestroyPreviewOverlay()
{
	if (!previewOverlayWidget_.IsValid())
	{
		return;
	}

	if (GEngine && GEngine->GameViewport)
	{
		GEngine->GameViewport->RemoveViewportWidgetContent(previewOverlayWidget_.ToSharedRef());
	}

	previewOverlaySlot_ = nullptr;
	previewOverlayWidget_.Reset();
	previewBox_.Reset();
	previewBrush_.Reset();
}

void AViewSynthesizer::UpdatePreviewOverlay()
{
	if (!GetWorld() || !GetWorld()->IsGameWorld())
	{
		return;
	}

	if (!previewOverlayWidget_.IsValid())
	{
		CreatePreviewOverlay();
	}

	if (!previewOverlayWidget_.IsValid())
	{
		return;
	}

	UpdatePreviewOverlayLayout();
	previewOverlayWidget_->SetVisibility(EVisibility::HitTestInvisible);
}

void AViewSynthesizer::UpdatePreviewOverlayLayout()
{
	if (!resultRenderTarget_ || !previewBrush_.IsValid())
	{
		return;
	}

	FVector2D viewportSize;
	if (!TryGetPreviewViewportSize(viewportSize))
	{
		return;
	}

	float renderTargetWidth = static_cast<float>(resultRenderTarget_->SizeX);
	float renderTargetHeight = static_cast<float>(resultRenderTarget_->SizeY);
	if (renderTargetWidth <= 0.0f || renderTargetHeight <= 0.0f)
	{
		renderTargetWidth = 1.0f;
		renderTargetHeight = 1.0f;
	}

	const float maxPreviewWidth = FMath::Max(0.0f, FMath::Min(viewportSize.X, viewportSize.Y * renderTargetWidth / renderTargetHeight));
	const float previewWidth = maxPreviewWidth * previewSizePercent_ / PREVIEW_PERCENT_MAX;
	const float previewHeight = previewWidth * renderTargetHeight / renderTargetWidth;
	const float desiredX = viewportSize.X * previewXPercent_ / PREVIEW_PERCENT_MAX;
	const float desiredY = viewportSize.Y * previewYPercent_ / PREVIEW_PERCENT_MAX;
	const float clampedX = FMath::Clamp(desiredX, 0.0f, FMath::Max(0.0f, viewportSize.X - previewWidth));
	const float clampedY = FMath::Clamp(desiredY, 0.0f, FMath::Max(0.0f, viewportSize.Y - previewHeight));

	previewBrush_->ImageSize = FVector2D(previewWidth, previewHeight);

	if (previewBox_.IsValid())
	{
		previewBox_->SetWidthOverride(previewWidth);
		previewBox_->SetHeightOverride(previewHeight);
	}

	if (previewOverlaySlot_)
	{
		previewOverlaySlot_->SetPadding(FMargin(clampedX, clampedY, 0.0f, 0.0f));
	}
}

void AViewSynthesizer::UpdatePreviewLayoutControls()
{
	if (!previewXSpinBox_.IsValid() || !previewYSpinBox_.IsValid() || !previewSizeSpinBox_.IsValid())
	{
		return;
	}

	previewLayoutControlsSyncInProgress_ = true;
	previewXSpinBox_->SetValue(previewXPercent_);
	previewYSpinBox_->SetValue(previewYPercent_);
	previewSizeSpinBox_->SetValue(previewSizePercent_);
	previewLayoutControlsSyncInProgress_ = false;
}

void AViewSynthesizer::TryInstallPreviewLayoutControls()
{
	if (!GetWorld() || !GetWorld()->IsGameWorld())
	{
		return;
	}

	UUserWidget* controlWidget = FindViewSynthesisControlWidget();
	if (!controlWidget)
	{
		ResetPreviewLayoutWidgetState();
		return;
	}

	if (AViewSynthesizer* controlledSynthesizer = ResolveControlledSynthesizer(controlWidget))
	{
		if (controlledSynthesizer != this)
		{
			return;
		}
	}

	UWidgetTree* widgetTree = controlWidget->WidgetTree;
	if (!widgetTree)
	{
		return;
	}

	if (!previewXSpinBox_.IsValid())
	{
		previewXSpinBox_ = Cast<USpinBox>(widgetTree->FindWidget(FName(PREVIEW_X_SPIN_BOX_WIDGET_NAME)));
	}

	if (!previewYSpinBox_.IsValid())
	{
		previewYSpinBox_ = Cast<USpinBox>(widgetTree->FindWidget(FName(PREVIEW_Y_SPIN_BOX_WIDGET_NAME)));
	}

	if (!previewSizeSpinBox_.IsValid())
	{
		previewSizeSpinBox_ = Cast<USpinBox>(widgetTree->FindWidget(FName(PREVIEW_SIZE_SPIN_BOX_WIDGET_NAME)));
	}

	if (!previewXSpinBox_.IsValid() || !previewYSpinBox_.IsValid() || !previewSizeSpinBox_.IsValid())
	{
		UVerticalBox* container = nullptr;
		int32 largestContainerChildCount = INDEX_NONE;

		TArray<UWidget*> allWidgets;
		widgetTree->GetAllWidgets(allWidgets);
		for (UWidget* widget : allWidgets)
		{
			UVerticalBox* candidate = Cast<UVerticalBox>(widget);
			if (!candidate)
			{
				continue;
			}

			const int32 childCount = candidate->GetChildrenCount();
			if (childCount > largestContainerChildCount)
			{
				container = candidate;
				largestContainerChildCount = childCount;
			}
		}

		if (!container)
		{
			return;
		}

		UVerticalBox* previewSection = widgetTree->ConstructWidget<UVerticalBox>(UVerticalBox::StaticClass(), FName(PREVIEW_LAYOUT_SECTION_WIDGET_NAME));
		UTextBlock* sectionHeader = widgetTree->ConstructWidget<UTextBlock>(UTextBlock::StaticClass());
		sectionHeader->SetText(FText::FromString(TEXT("Preview Layout")));

		if (UVerticalBoxSlot* headerSlot = previewSection->AddChildToVerticalBox(sectionHeader))
		{
			headerSlot->SetPadding(FMargin(0.0f, PREVIEW_LAYOUT_SECTION_PADDING, 0.0f, PREVIEW_LAYOUT_ROW_PADDING));
		}

		auto AddPreviewSpinBoxRow = [&](const TCHAR* spinBoxWidgetName, const TCHAR* labelText) -> USpinBox*
		{
			UHorizontalBox* row = widgetTree->ConstructWidget<UHorizontalBox>(UHorizontalBox::StaticClass());
			USizeBox* labelBox = widgetTree->ConstructWidget<USizeBox>(USizeBox::StaticClass());
			labelBox->SetWidthOverride(PREVIEW_LAYOUT_LABEL_WIDTH);

			UTextBlock* label = widgetTree->ConstructWidget<UTextBlock>(UTextBlock::StaticClass());
			label->SetText(FText::FromString(labelText));
			labelBox->AddChild(label);

			if (UHorizontalBoxSlot* labelSlot = row->AddChildToHorizontalBox(labelBox))
			{
				labelSlot->SetVerticalAlignment(VAlign_Center);
			}

			USpinBox* spinBox = widgetTree->ConstructWidget<USpinBox>(USpinBox::StaticClass(), FName(spinBoxWidgetName));
			spinBox->SetMinValue(PREVIEW_PERCENT_MIN);
			spinBox->SetMaxValue(PREVIEW_PERCENT_MAX);
			spinBox->SetDelta(PREVIEW_SPIN_BOX_DELTA);
			spinBox->SetMinFractionalDigits(1);
			spinBox->SetMaxFractionalDigits(1);

			if (UHorizontalBoxSlot* spinBoxSlot = row->AddChildToHorizontalBox(spinBox))
			{
				spinBoxSlot->SetSize(FSlateChildSize(ESlateSizeRule::Fill));
				spinBoxSlot->SetVerticalAlignment(VAlign_Center);
			}

			if (UVerticalBoxSlot* rowSlot = previewSection->AddChildToVerticalBox(row))
			{
				rowSlot->SetPadding(FMargin(0.0f, 0.0f, 0.0f, PREVIEW_LAYOUT_ROW_PADDING));
			}

			return spinBox;
		};

		previewXSpinBox_ = AddPreviewSpinBoxRow(PREVIEW_X_SPIN_BOX_WIDGET_NAME, TEXT("Preview X (%)"));
		previewYSpinBox_ = AddPreviewSpinBoxRow(PREVIEW_Y_SPIN_BOX_WIDGET_NAME, TEXT("Preview Y (%)"));
		previewSizeSpinBox_ = AddPreviewSpinBoxRow(PREVIEW_SIZE_SPIN_BOX_WIDGET_NAME, TEXT("Preview Size (%)"));

		if (UVerticalBoxSlot* sectionSlot = container->AddChildToVerticalBox(previewSection))
		{
			sectionSlot->SetPadding(FMargin(0.0f, PREVIEW_LAYOUT_SECTION_PADDING, 0.0f, 0.0f));
		}
	}

	if (previewXSpinBox_.IsValid())
	{
		previewXSpinBox_->OnValueChanged.AddUniqueDynamic(this, &AViewSynthesizer::HandlePreviewXSpinBoxValueChanged);
	}

	if (previewYSpinBox_.IsValid())
	{
		previewYSpinBox_->OnValueChanged.AddUniqueDynamic(this, &AViewSynthesizer::HandlePreviewYSpinBoxValueChanged);
	}

	if (previewSizeSpinBox_.IsValid())
	{
		previewSizeSpinBox_->OnValueChanged.AddUniqueDynamic(this, &AViewSynthesizer::HandlePreviewSizeSpinBoxValueChanged);
	}

	UpdatePreviewLayoutControls();
}

UUserWidget* AViewSynthesizer::FindViewSynthesisControlWidget()
{
	if (viewSynthesisControlWidget_.IsValid())
	{
		return viewSynthesisControlWidget_.Get();
	}

	if (!viewSynthesisControlWidgetClassLookupAttempted_)
	{
		viewSynthesisControlWidgetClassLookupAttempted_ = true;
		viewSynthesisControlWidgetClass_ = LoadClass<UUserWidget>(nullptr, VIEW_SYNTHESIS_CONTROL_WIDGET_CLASS_PATH);
	}

	if (!viewSynthesisControlWidgetClass_)
	{
		return nullptr;
	}

	TArray<UUserWidget*> widgets;
	UWidgetBlueprintLibrary::GetAllWidgetsOfClass(this, widgets, viewSynthesisControlWidgetClass_, false);

	for (UUserWidget* widget : widgets)
	{
		if (ResolveControlledSynthesizer(widget) == this)
		{
			viewSynthesisControlWidget_ = widget;
			return widget;
		}
	}

	if (widgets.Num() == 1 && !ResolveControlledSynthesizer(widgets[0]))
	{
		viewSynthesisControlWidget_ = widgets[0];
		return widgets[0];
	}

	return nullptr;
}

AViewSynthesizer* AViewSynthesizer::ResolveControlledSynthesizer(UUserWidget* widget) const
{
	if (!widget)
	{
		return nullptr;
	}

	const FObjectProperty* controlledSynthesizerProperty = FindFProperty<FObjectProperty>(widget->GetClass(), CONTROLLED_SYNTHESIZER_PROPERTY_NAME);
	if (!controlledSynthesizerProperty)
	{
		return nullptr;
	}

	return Cast<AViewSynthesizer>(controlledSynthesizerProperty->GetObjectPropertyValue_InContainer(widget));
}

void AViewSynthesizer::ResetPreviewLayoutWidgetState()
{
	viewSynthesisControlWidget_.Reset();
	previewXSpinBox_.Reset();
	previewYSpinBox_.Reset();
	previewSizeSpinBox_.Reset();
}

float AViewSynthesizer::ClampPreviewPercent(float value) const
{
	return FMath::Clamp(value, PREVIEW_PERCENT_MIN, PREVIEW_PERCENT_MAX);
}

void AViewSynthesizer::ApplyPreviewLayout(float xPercent, float yPercent, float sizePercent)
{
	previewXPercent_ = ClampPreviewPercent(xPercent);
	previewYPercent_ = ClampPreviewPercent(yPercent);
	previewSizePercent_ = ClampPreviewPercent(sizePercent);
	UpdatePreviewOverlayLayout();
	UpdatePreviewLayoutControls();
}

void AViewSynthesizer::ApplyLegacyPreviewLayoutDefaults()
{
	FVector2D viewportSize;
	if (!TryGetPreviewViewportSize(viewportSize))
	{
		viewportSize = FVector2D(LEGACY_FALLBACK_VIEWPORT_WIDTH, LEGACY_FALLBACK_VIEWPORT_HEIGHT);
	}

	float renderTargetWidth = static_cast<float>(resultRenderTarget_ ? resultRenderTarget_->SizeX : 0);
	float renderTargetHeight = static_cast<float>(resultRenderTarget_ ? resultRenderTarget_->SizeY : 0);
	if (renderTargetWidth <= 0.0f || renderTargetHeight <= 0.0f)
	{
		renderTargetWidth = 1.0f;
		renderTargetHeight = 1.0f;
	}

	const float maxPreviewWidth = FMath::Max(0.0f, FMath::Min(viewportSize.X, viewportSize.Y * renderTargetWidth / renderTargetHeight));
	const float legacyXPercent = viewportSize.X > 0.0f ? (LEGACY_PREVIEW_MARGIN / viewportSize.X) * PREVIEW_PERCENT_MAX : 0.0f;
	const float legacyYPercent = viewportSize.Y > 0.0f ? (LEGACY_PREVIEW_MARGIN / viewportSize.Y) * PREVIEW_PERCENT_MAX : 0.0f;
	const float legacySizePercent = maxPreviewWidth > 0.0f ? (LEGACY_PREVIEW_WIDTH / maxPreviewWidth) * PREVIEW_PERCENT_MAX : PREVIEW_PERCENT_MAX;

	ApplyPreviewLayout(legacyXPercent, legacyYPercent, legacySizePercent);
}

void AViewSynthesizer::QueuePreviewLayoutSave()
{
	if (!GetWorld())
	{
		return;
	}

	previewLayoutSavePending_ = true;
	previewLayoutLastUiChangeTime_ = static_cast<double>(GetWorld()->GetRealTimeSeconds());
}

void AViewSynthesizer::FlushPendingPreviewLayoutSave()
{
	if (!previewLayoutSavePending_ || !GetWorld())
	{
		return;
	}

	const double secondsSinceLastChange = static_cast<double>(GetWorld()->GetRealTimeSeconds()) - previewLayoutLastUiChangeTime_;
	if (secondsSinceLastChange < PREVIEW_LAYOUT_SAVE_DEBOUNCE_SECONDS)
	{
		return;
	}

	SavePreviewLayout();
}

bool AViewSynthesizer::TryGetPreviewViewportSize(FVector2D& outViewportSize) const
{
	if (!GEngine || !GEngine->GameViewport || !GEngine->GameViewport->Viewport)
	{
		return false;
	}

	const FIntPoint viewportSize = GEngine->GameViewport->Viewport->GetSizeXY();
	if (viewportSize.X <= 0 || viewportSize.Y <= 0)
	{
		return false;
	}

	outViewportSize = FVector2D(viewportSize);
	return true;
}

void AViewSynthesizer::HandlePreviewXSpinBoxValueChanged(float value)
{
	if (previewLayoutControlsSyncInProgress_)
	{
		return;
	}

	SetPreviewXPercent(value);
	QueuePreviewLayoutSave();
}

void AViewSynthesizer::HandlePreviewYSpinBoxValueChanged(float value)
{
	if (previewLayoutControlsSyncInProgress_)
	{
		return;
	}

	SetPreviewYPercent(value);
	QueuePreviewLayoutSave();
}

void AViewSynthesizer::HandlePreviewSizeSpinBoxValueChanged(float value)
{
	if (previewLayoutControlsSyncInProgress_)
	{
		return;
	}

	SetPreviewSizePercent(value);
	QueuePreviewLayoutSave();
}

UUserWidget* AViewSynthesizer::FindMainMenuWidget()
{
    if (mainMenuWidget_.IsValid())
    {
        return mainMenuWidget_.Get();
    }

    if (!mainMenuWidgetClassLookupAttempted_)
    {
        mainMenuWidgetClassLookupAttempted_ = true;
        mainMenuWidgetClass_ = LoadClass<UUserWidget>(nullptr, MAIN_MENU_WIDGET_CLASS_PATH);
    }

    if (!mainMenuWidgetClass_)
    {
        return nullptr;
    }

    TArray<UUserWidget*> widgets;
    UWidgetBlueprintLibrary::GetAllWidgetsOfClass(this, widgets, mainMenuWidgetClass_, false);

    if (widgets.Num() <= 0)
    {
        return nullptr;
    }

    mainMenuWidgetObserved_ = true;
    mainMenuWidget_ = widgets[0];

    return widgets[0];
}

bool AViewSynthesizer::IsMainMenuOpen()
{
    if (UUserWidget* mainMenuWidget = FindMainMenuWidget())
    {
        return mainMenuWidget->IsInViewport() && mainMenuWidget->IsVisible();
    }

    return !mainMenuWidgetObserved_ && GetGameTimeSinceCreation() < MAIN_MENU_GRACE_PERIOD_SECONDS;
}
