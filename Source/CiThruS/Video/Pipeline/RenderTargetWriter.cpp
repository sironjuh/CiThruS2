#include "RenderTargetWriter.h"
#include "PipelineSource.h"
#include "Misc/Debug.h"
#include "Engine/Texture2D.h"
#include "Engine/TextureRenderTarget2D.h"

#include <chrono>
#include <cstring>

RenderTargetWriter::RenderTargetWriter(UTextureRenderTarget2D* texture)
	: inputBuffer_(nullptr), texture_(nullptr), frameWidth_(0), frameHeight_(0), bytesPerPixel_(0), frameDirty_(false), initialized_(false), destroyed_(false)
{
	if (!texture)
	{
		Debug::Log("No texture provided to RenderTargetWriter");
		return;
	}

	ETextureRenderTargetFormat format = texture->RenderTargetFormat;

	switch (format)
	{
	case ETextureRenderTargetFormat::RTF_R32f:
		GetInputPin<0>().SetAcceptedFormat("gray32f");
		bytesPerPixel_ = 4 * 1;
		break;
	case ETextureRenderTargetFormat::RTF_RGBA32f:
		GetInputPin<0>().SetAcceptedFormat("rgba32f");
		bytesPerPixel_ = 4 * 4;
		break;
	case ETextureRenderTargetFormat::RTF_RGBA8:
	case ETextureRenderTargetFormat::RTF_RGBA8_SRGB:
		// For some reason UE actually expects BGRA, not RGBA...
		GetInputPin<0>().SetAcceptedFormat("bgra");
		bytesPerPixel_ = 1 * 4;
		break;
	default:
		return;
	}

	// Note that this is executed on the render thread later and not yet
	BeginRenderCommand();
	ENQUEUE_RENDER_COMMAND(InitializeReader)(
		[this, texture](FRHICommandListImmediate& RHICmdList)
		{
			resourceMutex_.lock();

			if (destroyed_)
			{
				resourceMutex_.unlock();
				EndRenderCommand();
				return;
			}

			texture_ = texture->GetResource()->GetTexture2DRHI();
			frameWidth_ = texture_->GetDesc().Extent.X;
			frameHeight_ = texture_->GetDesc().Extent.Y;
			inputBuffer_ = new uint8_t[frameWidth_ * frameHeight_ * bytesPerPixel_];
			initialized_ = true;

			resourceMutex_.unlock();
			EndRenderCommand();
		});
}

RenderTargetWriter::~RenderTargetWriter()
{
	{
		std::lock_guard<std::mutex> resourceLock(resourceMutex_);
		destroyed_ = true;
		initialized_ = false;
	}

	{
		std::unique_lock<std::mutex> renderCommandLock(renderCommandMutex_);
		renderCommandCv_.wait(renderCommandLock, [this] { return pendingRenderCommandCount_ == 0; });
	}

	std::lock_guard<std::mutex> resourceLock(resourceMutex_);

	delete[] inputBuffer_;
	inputBuffer_ = nullptr;

	texture_ = nullptr;
}

void RenderTargetWriter::Process()
{
	const uint8_t* inputData = GetInputPin<0>().GetData();
	uint32_t inputSize = GetInputPin<0>().GetSize();

	if (!initialized_ || !inputData || inputSize != frameWidth_ * frameHeight_ * bytesPerPixel_)
	{
		return;
	}

	writeMutex_.lock();

	// We should only pass new data to the render thread if the render thread has processed the previous data!
	// Otherwise the render commands may pile up faster than UE can execute them and the program hangs
	if (frameDirty_)
	{
		writeMutex_.unlock();
		RecordDroppedFrame();
		return;
	}

	// Input data must be grabbed now, there's no guarantee inputFrame_ will be valid after exiting this function
	const auto copyStart = std::chrono::steady_clock::now();
	std::memcpy(inputBuffer_, inputData, inputSize);
	const auto copyEnd = std::chrono::steady_clock::now();
	RecordCopySample(std::chrono::duration<double, std::milli>(copyEnd - copyStart).count());

	frameDirty_ = true;

	writeMutex_.unlock();

	const auto enqueueTime = std::chrono::steady_clock::now();

	// Note that this is executed on the render thread later and not yet
	BeginRenderCommand();
	ENQUEUE_RENDER_COMMAND(ExtractRenderTargets)(
		[this, enqueueTime](FRHICommandListImmediate& RHICmdList)
		{
			resourceMutex_.lock();

			if (!initialized_)
			{
				resourceMutex_.unlock();
				EndRenderCommand();
				return;
			}

			// Write the contents of the texture
			FUpdateTextureRegion2D updateRegion = {};

			updateRegion.SrcX = 0;
			updateRegion.SrcY = 0;
			updateRegion.DestX = 0;
			updateRegion.DestY = 0;
			updateRegion.Width = frameWidth_;
			updateRegion.Height = frameHeight_;

			writeMutex_.lock();

			const auto renderStart = std::chrono::steady_clock::now();
			const double queueWaitMs = std::chrono::duration<double, std::milli>(renderStart - enqueueTime).count();
			const auto updateStart = renderStart;
			RHICmdList.UpdateTexture2D(texture_, 0, updateRegion, frameWidth_ * bytesPerPixel_, inputBuffer_);
			const auto updateEnd = std::chrono::steady_clock::now();
			const double updateMs = std::chrono::duration<double, std::milli>(updateEnd - updateStart).count();
			const double endToEndMs = std::chrono::duration<double, std::milli>(updateEnd - enqueueTime).count();

			frameDirty_ = false;

			writeMutex_.unlock();

			resourceMutex_.unlock();
			EndRenderCommand();
			RecordRenderSamples(queueWaitMs, updateMs, endToEndMs);
		});
}

void RenderTargetWriter::BeginRenderCommand()
{
	std::lock_guard<std::mutex> renderCommandLock(renderCommandMutex_);
	++pendingRenderCommandCount_;
}

void RenderTargetWriter::EndRenderCommand()
{
	std::lock_guard<std::mutex> renderCommandLock(renderCommandMutex_);
	if (pendingRenderCommandCount_ > 0)
	{
		--pendingRenderCommandCount_;
	}
	if (pendingRenderCommandCount_ == 0)
	{
		renderCommandCv_.notify_all();
	}
}

void RenderTargetWriter::RecordCopySample(double milliseconds)
{
	std::lock_guard<std::mutex> perfLock(perfMutex_);
	uploadCopyMsTotal_ += milliseconds;
	++uploadCopySamples_;
}

void RenderTargetWriter::RecordDroppedFrame(uint64_t count)
{
	std::lock_guard<std::mutex> perfLock(perfMutex_);
	droppedFrames_ += count;
}

void RenderTargetWriter::RecordRenderSamples(double queueWaitMs, double updateMs, double endToEndMs)
{
	uint32_t perfFrames = 0;
	double avgCopyMs = 0.0;
	double avgQueueWaitMs = 0.0;
	double avgUpdateMs = 0.0;
	double avgEndToEndMs = 0.0;
	uint64_t droppedFrames = 0;
	bool shouldLog = false;

	{
		std::lock_guard<std::mutex> perfLock(perfMutex_);
		uploadQueueWaitMsTotal_ += queueWaitMs;
		uploadUpdateMsTotal_ += updateMs;
		uploadEndToEndMsTotal_ += endToEndMs;
		++uploadQueueWaitSamples_;
		++uploadUpdateSamples_;
		++uploadEndToEndSamples_;
		++perfFramesAccumulated_;

		if (perfFramesAccumulated_ >= perfLogInterval_)
		{
			perfFrames = perfFramesAccumulated_;
			avgCopyMs = uploadCopySamples_ > 0 ? (uploadCopyMsTotal_ / static_cast<double>(uploadCopySamples_)) : 0.0;
			avgQueueWaitMs = uploadQueueWaitSamples_ > 0 ? (uploadQueueWaitMsTotal_ / static_cast<double>(uploadQueueWaitSamples_)) : 0.0;
			avgUpdateMs = uploadUpdateSamples_ > 0 ? (uploadUpdateMsTotal_ / static_cast<double>(uploadUpdateSamples_)) : 0.0;
			avgEndToEndMs = uploadEndToEndSamples_ > 0 ? (uploadEndToEndMsTotal_ / static_cast<double>(uploadEndToEndSamples_)) : 0.0;
			droppedFrames = droppedFrames_;

			perfFramesAccumulated_ = 0;
			uploadCopyMsTotal_ = 0.0;
			uploadQueueWaitMsTotal_ = 0.0;
			uploadUpdateMsTotal_ = 0.0;
			uploadEndToEndMsTotal_ = 0.0;
			uploadCopySamples_ = 0;
			uploadQueueWaitSamples_ = 0;
			uploadUpdateSamples_ = 0;
			uploadEndToEndSamples_ = 0;
			droppedFrames_ = 0;
			shouldLog = true;
		}
	}

	if (!shouldLog)
	{
		return;
	}

	uint32_t pendingCommands = 0;
	{
		std::lock_guard<std::mutex> renderCommandLock(renderCommandMutex_);
		pendingCommands = pendingRenderCommandCount_;
	}

	UE_LOG(
		LogTemp,
		Log,
		TEXT("RenderTargetWriter PERF [%ux%u bpp=%u frames=%u]: copy=%.2fms queue=%.2fms upload=%.2fms endToEnd=%.2fms pending=%u dropped=%llu"),
		static_cast<uint32_t>(frameWidth_),
		static_cast<uint32_t>(frameHeight_),
		static_cast<uint32_t>(bytesPerPixel_),
		perfFrames,
		avgCopyMs,
		avgQueueWaitMs,
		avgUpdateMs,
		avgEndToEndMs,
		pendingCommands,
		static_cast<unsigned long long>(droppedFrames));
}
