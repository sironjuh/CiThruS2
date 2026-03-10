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
#include "Misc/Debug.h"

#include "RHI.h"
#include "Blueprint/UserWidget.h"
#include "Blueprint/WidgetBlueprintLibrary.h"
#include "Engine/Engine.h"
#include "Engine/GameViewportClient.h"
#include "Engine/TextureRenderTarget2D.h"
#include "Components/SceneCaptureComponent2D.h"
#include "ShaderParameterStruct.h"
#include "DataDrivenShaderPlatformInfo.h"
#include "RenderResource.h"
#include "Styling/SlateBrush.h"
#include "Widgets/Images/SImage.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/SOverlay.h"

#include <algorithm>

namespace
{
constexpr TCHAR MAIN_MENU_WIDGET_CLASS_PATH[] = TEXT("/Game/UI/WBP_MainMenu.WBP_MainMenu_C");
constexpr float PREVIEW_MARGIN = 16.0f;
constexpr float PREVIEW_WIDTH = 320.0f;
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
    DestroyPreviewOverlay();

    Super::EndPlay(endPlayReason);

	DeleteStreams();
}

void AViewSynthesizer::Tick(float deltaTime)
{
	Super::Tick(deltaTime);

    UpdatePreviewOverlay();

    if (wantsStop_)
    {
        StopTransmitInternal();
    }

    const std::lock_guard<std::mutex> lock(streamMutex_);

    if (!transmitEnabled_)
    {
        return;
    }

    Capture();
}

void AViewSynthesizer::StartTransmit()
{
    const std::lock_guard<std::mutex> lock(streamMutex_);

    if (ResetStreams())
    {
        transmitEnabled_ = true;
        useEditorTick_ = true;
    }
}

void AViewSynthesizer::StopTransmit()
{
    // Stop the transmit in a synchronized manner to avoid race conditions
    wantsStop_ = true;
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

    wantsStop_ = false;
    frameNumber_ = 0;
    startTimestampMs_ = 0;

    frontCamera_->FOVAngle = frontFov_;
    rearCamera_->FOVAngle = rearFov_;

    frontRenderTarget_->ResizeTarget(frameWidth, frameHeight);
    rearRenderTarget_->ResizeTarget(frameWidth, frameHeight);
    resultRenderTarget_->ResizeTarget(frameWidth, frameHeight);
    UpdatePreviewOverlayDimensions();

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
                                        new HevcEncoder(frameWidth, frameHeight * 2, 16, quantizationParameter_, wavefrontParallelProcessing_, overlappedWavefront_, HevcPresetMinimumLatency)
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
                                            new HevcEncoder(frameWidth, frameHeight * 2, 16, quantizationParameter_, wavefrontParallelProcessing_, overlappedWavefront_, HevcPresetMinimumLatency),
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
}

void AViewSynthesizer::Capture()
{
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

        frontReader_->Read(reinterpret_cast<uint8_t*>(&frontCameraParams), sizeof(ViewSynthCameraParams));
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

        rearReader_->Read(reinterpret_cast<uint8_t*>(&rearCameraParams), sizeof(ViewSynthCameraParams));
    }

    frameNumber_++;
}

void AViewSynthesizer::CreatePreviewOverlay()
{
    if (previewOverlayWidget_.IsValid() || !resultRenderTarget_ || !GetWorld() || !GetWorld()->IsGameWorld() || !GEngine || !GEngine->GameViewport)
    {
        return;
    }

    previewBrush_ = MakeShared<FSlateBrush>();
    previewBrush_->SetResourceObject(resultRenderTarget_);
    previewBrush_->DrawAs = ESlateBrushDrawType::Image;
    previewBrush_->TintColor = FSlateColor(FLinearColor::White);

    UpdatePreviewOverlayDimensions();

    GEngine->GameViewport->AddViewportWidgetContent(
        SAssignNew(previewOverlayWidget_, SOverlay)
        + SOverlay::Slot()
        .HAlign(HAlign_Left)
        .VAlign(VAlign_Top)
        .Padding(FMargin(PREVIEW_MARGIN))
        [
            SAssignNew(previewBox_, SBox)
            .WidthOverride(previewBrush_->ImageSize.X)
            .HeightOverride(previewBrush_->ImageSize.Y)
            [
                SNew(SImage)
                .Image(previewBrush_.Get())
            ]
        ],
        1);

    if (previewOverlayWidget_.IsValid())
    {
        previewOverlayWidget_->SetVisibility(EVisibility::Collapsed);
    }

    UpdatePreviewOverlay();
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

    previewOverlayWidget_.Reset();
    previewBox_.Reset();
    previewBrush_.Reset();
    previewLastRenderTargetWidth_ = 0;
    previewLastRenderTargetHeight_ = 0;
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

    UpdatePreviewOverlayDimensions();

    previewOverlayWidget_->SetVisibility(IsMainMenuOpen() ? EVisibility::Collapsed : EVisibility::Visible);
}

void AViewSynthesizer::UpdatePreviewOverlayDimensions()
{
    if (!resultRenderTarget_ || !previewBrush_.IsValid())
    {
        return;
    }

    const int32 renderTargetWidth = resultRenderTarget_->SizeX;
    const int32 renderTargetHeight = resultRenderTarget_->SizeY;

    if (renderTargetWidth <= 0 || renderTargetHeight <= 0)
    {
        previewBrush_->ImageSize = FVector2D(PREVIEW_WIDTH, PREVIEW_WIDTH);

        if (previewBox_.IsValid())
        {
            previewBox_->SetWidthOverride(PREVIEW_WIDTH);
            previewBox_->SetHeightOverride(PREVIEW_WIDTH);
        }

        return;
    }

    if (renderTargetWidth == previewLastRenderTargetWidth_ && renderTargetHeight == previewLastRenderTargetHeight_)
    {
        return;
    }

    previewLastRenderTargetWidth_ = renderTargetWidth;
    previewLastRenderTargetHeight_ = renderTargetHeight;

    const float previewHeight = PREVIEW_WIDTH * static_cast<float>(renderTargetHeight) / static_cast<float>(renderTargetWidth);
    previewBrush_->ImageSize = FVector2D(PREVIEW_WIDTH, previewHeight);

    if (previewBox_.IsValid())
    {
        previewBox_->SetWidthOverride(PREVIEW_WIDTH);
        previewBox_->SetHeightOverride(previewHeight);
    }
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
