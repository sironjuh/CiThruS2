#include "VideoTransmitter.h"

#include "Misc/Debug.h"
#include "Pipeline/AsyncPipelineRunner.h"
#include "Pipeline/BgraToRgbaConverter.h"
#include "Pipeline/Equirectangular360Converter.h"
#include "Pipeline/FileSink.h"
#include "Pipeline/HevcEncoder.h"
#include "Pipeline/Pipeline.h"
#include "Pipeline/PngRecorder.h"
#include "Pipeline/RenderTargetReader.h"
#include "Pipeline/RgbaToYuvConverter.h"
#include "Pipeline/RtpTransmitter.h"
#include "Pipeline/ScaffoldingSequentialFilter.h"
#include "Pipeline/SolidColorImageGenerator.h"

#include <algorithm>
#include <string>

AVideoTransmitter::AVideoTransmitter()
{
	RootComponent = CreateDefaultSubobject<USceneComponent>(TEXT("RootComponent"));

	static const FRotator CAMERA_ROTATIONS[] =
	{
		FRotator(90.0f, 0.0f, 0.0f),
		FRotator(0.0f, -90.0f, 0.0f),
		FRotator(0.0f, 0.0f, 0.0f),
		FRotator(0.0f, 90.0f, 0.0f),
		FRotator(0.0f, 180.0f, 0.0f),
		FRotator(-90.0f, 0.0f, 0.0f)
	};

	for (int i = 0; i < 6; i++)
	{
		USceneCaptureComponent2D* sceneCaptureComponent = CreateDefaultSubobject<USceneCaptureComponent2D>(FName("360SceneCaptureComponent" + FString::FromInt(i)));
		sceneCaptureComponent->SetupAttachment(RootComponent);
		sceneCaptureComponent->SetRelativeRotation(CAMERA_ROTATIONS[i]);

		cubemapCameras_.Add(sceneCaptureComponent);
	}

	normalCamera_ = CreateDefaultSubobject<USceneCaptureComponent2D>(TEXT("PerspectiveSceneCaptureComponent"));
	normalCamera_->SetupAttachment(RootComponent);

	runner_ = nullptr;
	reader_ = nullptr;
	wantsStop_ = false;
	transmitEnabled_ = false;
	capture360_ = false;
	useEditorTick_ = false;
	captureAccumulator_ = 0.0;

	PrimaryActorTick.bCanEverTick = true;
}

void AVideoTransmitter::PostRegisterAllComponents()
{
	Super::PostRegisterAllComponents();

	static const uint16_t RENDER_TARGET_DEFAULT_RESOLUTION = 512;

	for (USceneCaptureComponent2D* camera : cubemapCameras_)
	{
		camera->TextureTarget = NewObject<UTextureRenderTarget2D>();
		camera->TextureTarget->InitCustomFormat(RENDER_TARGET_DEFAULT_RESOLUTION, RENDER_TARGET_DEFAULT_RESOLUTION, PF_B8G8R8A8, false);
		camera->TextureTarget->RenderTargetFormat = RTF_RGBA8_SRGB;
		camera->bCaptureEveryFrame = false;
		camera->bCaptureOnMovement = false;
	}

	normalCamera_->TextureTarget = NewObject<UTextureRenderTarget2D>();
	normalCamera_->TextureTarget->InitCustomFormat(RENDER_TARGET_DEFAULT_RESOLUTION, RENDER_TARGET_DEFAULT_RESOLUTION, PF_B8G8R8A8, false);
	normalCamera_->TextureTarget->RenderTargetFormat = RTF_RGBA8_SRGB;
	normalCamera_->bCaptureEveryFrame = false;
	normalCamera_->bCaptureOnMovement = false;
}

void AVideoTransmitter::EndPlay(const EEndPlayReason::Type endPlayReason)
{
	Super::EndPlay(endPlayReason);
	DeleteStreams();
}

void AVideoTransmitter::Tick(float deltaTime)
{
	Super::Tick(deltaTime);

	if (wantsStop_)
	{
		StopTransmitInternal();
	}

	const std::lock_guard<std::mutex> lock(streamMutex_);
	if (!transmitEnabled_ || !reader_)
	{
		return;
	}

	if (dropFramesWhenBusy_ && reader_->IsBusy())
	{
		return;
	}

	if (!capture360_ && maxStreamFps_ > 0)
	{
		const double frameInterval = 1.0 / static_cast<double>(maxStreamFps_);
		captureAccumulator_ += static_cast<double>(deltaTime);
		if (captureAccumulator_ < frameInterval)
		{
			return;
		}
		captureAccumulator_ = std::min(captureAccumulator_ - frameInterval, frameInterval);
	}

	if (capture360_)
	{
		for (USceneCaptureComponent2D* camera : cubemapCameras_)
		{
			camera->CaptureScene();
		}
	}
	else
	{
		normalCamera_->CaptureScene();
	}

	if (dropFramesWhenBusy_)
	{
		reader_->TryRead();
	}
	else
	{
		reader_->Read();
	}
}

void AVideoTransmitter::StartTransmit()
{
	std::lock_guard<std::mutex> lock(streamMutex_);

	if (ResetStreams())
	{
		captureAccumulator_ = 0.0;
		transmitEnabled_ = true;
		useEditorTick_ = true;
		wantsStop_ = false;
	}
}

void AVideoTransmitter::StopTransmit()
{
	wantsStop_ = true;
}

bool AVideoTransmitter::StartStreams()
{
	if (saveDirectory_.IsEmpty() || saveDirectory_.Contains("\\") || saveDirectory_[saveDirectory_.Len() - 1] != '/')
	{
		Debug::Log("Invalid directory");
		return false;
	}

	uint16_t frameWidth = std::max(remoteStreamWidth_, 16);
	uint16_t frameHeight = std::max(remoteStreamHeight_, 16);
	frameWidth += (8 - (frameWidth % 8)) % 8;
	frameHeight += (8 - (frameHeight % 8)) % 8;

	const uint32_t expectedStreamFps = maxStreamFps_ > 0 ? static_cast<uint32_t>(maxStreamFps_) : 60u;
	const EHevcEncoderBackend resolvedBackend = HevcEncoder::ResolveBackend(hevcEncoderBackend_);
	capture360_ = enable360Capture_;

	try
	{
		if (enable360Capture_)
		{
			for (USceneCaptureComponent2D* camera : cubemapCameras_)
			{
				camera->TextureTarget->ResizeTarget(widthAndHeightPerCaptureSide_, widthAndHeightPerCaptureSide_);
			}

			std::vector<UTextureRenderTarget2D*> renderTargets;
			renderTargets.resize(6, nullptr);
			for (int i = 0; i < 6; i++)
			{
				renderTargets[i] = cubemapCameras_[i]->TextureTarget;
			}

			reader_ = new RenderTargetReader(renderTargets);

			if (saveToFile_)
			{
				runner_ = new AsyncPipelineRunner(
					new Pipeline(
						reader_,
						new ImageSequentialFilter(
							{
								new Equirectangular360Converter(widthAndHeightPerCaptureSide_, widthAndHeightPerCaptureSide_, frameWidth, frameHeight, bilinearFiltering_),
								new BgraToRgbaConverter(),
							}),
						new PngRecorder(TCHAR_TO_UTF8(*saveDirectory_), frameWidth, frameHeight)));
			}
			else
			{
				runner_ = new AsyncPipelineRunner(
					new Pipeline(
						reader_,
						new ImageSequentialFilter(
							{
								new Equirectangular360Converter(widthAndHeightPerCaptureSide_, widthAndHeightPerCaptureSide_, frameWidth, frameHeight, bilinearFiltering_),
								new RgbaToYuvConverter(frameWidth, frameHeight),
								new HevcEncoder(
									frameWidth,
									frameHeight,
									processingThreadCount_,
									quantizationParameter_,
									wavefrontParallelProcessing_,
									overlappedWavefront_,
									saveToFile_ ? HevcPresetLossless : HevcPresetMinimumLatency,
									hevcEncoderBackend_,
									targetBitrateMbps_,
									static_cast<uint32_t>(maxKeyFrameInterval_),
									expectedStreamFps)
							}),
						new RtpTransmitter(TCHAR_TO_UTF8(*remoteStreamIp_), remoteVideoDstPort_)));
			}
		}
		else
		{
			normalCamera_->FOVAngle = fov_;
			normalCamera_->TextureTarget->ResizeTarget(frameWidth, frameHeight);

			std::vector<UTextureRenderTarget2D*> renderTargets = { normalCamera_->TextureTarget };
			reader_ = new RenderTargetReader(renderTargets);

			if (saveToFile_)
			{
				runner_ = new AsyncPipelineRunner(
					new Pipeline(
						reader_,
						new ImageSequentialFilter(
							{
								new BgraToRgbaConverter(),
							}),
						new PngRecorder(TCHAR_TO_UTF8(*saveDirectory_), frameWidth, frameHeight)));
			}
			else if (resolvedBackend == EHevcEncoderBackend::VideoToolbox)
			{
				runner_ = new AsyncPipelineRunner(
					new Pipeline(
						reader_,
						new HevcEncoder(
							frameWidth,
							frameHeight,
							processingThreadCount_,
							quantizationParameter_,
							wavefrontParallelProcessing_,
							overlappedWavefront_,
							HevcPresetMinimumLatency,
							hevcEncoderBackend_,
							targetBitrateMbps_,
							static_cast<uint32_t>(maxKeyFrameInterval_),
							expectedStreamFps),
						new RtpTransmitter(TCHAR_TO_UTF8(*remoteStreamIp_), remoteVideoDstPort_)));
			}
			else
			{
				runner_ = new AsyncPipelineRunner(
					new Pipeline(
						reader_,
						new ImageSequentialFilter(
							{
								new RgbaToYuvConverter(frameWidth, frameHeight),
								new HevcEncoder(
									frameWidth,
									frameHeight,
									processingThreadCount_,
									quantizationParameter_,
									wavefrontParallelProcessing_,
									overlappedWavefront_,
									HevcPresetMinimumLatency,
									hevcEncoderBackend_,
									targetBitrateMbps_,
									static_cast<uint32_t>(maxKeyFrameInterval_),
									expectedStreamFps)
							}),
						new RtpTransmitter(TCHAR_TO_UTF8(*remoteStreamIp_), remoteVideoDstPort_)));
			}
		}
	}
	catch (const std::exception& exception)
	{
		Debug::Log("Pipeline construction failed: " + std::string(exception.what()));
		DeleteStreams();
		return false;
	}

	return true;
}

bool AVideoTransmitter::ResetStreams()
{
	DeleteStreams();
	return StartStreams();
}

void AVideoTransmitter::DeleteStreams()
{
	delete runner_;
	runner_ = nullptr;
	reader_ = nullptr;
	captureAccumulator_ = 0.0;
}

void AVideoTransmitter::StopTransmitInternal()
{
	std::lock_guard<std::mutex> lock(streamMutex_);
	DeleteStreams();
	transmitEnabled_ = false;
	useEditorTick_ = false;
	wantsStop_ = false;
	captureAccumulator_ = 0.0;
}
