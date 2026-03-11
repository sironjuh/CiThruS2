#include "RenderTargetWriter.h"
#include "PipelineSource.h"
#include "Misc/Debug.h"
#include "Engine/Texture2D.h"
#include "Engine/TextureRenderTarget2D.h"

#include <chrono>
#include <cstring>

RenderTargetWriter::RenderTargetWriter(UTextureRenderTarget2D* texture)
	: frameBuffers_{ nullptr, nullptr }
	, texture_(nullptr)
	, frameWidth_(0)
	, frameHeight_(0)
	, bytesPerPixel_(0)
	, pendingFrameAvailable_(false)
	, pendingBufferIndex_(0)
	, uploadBufferIndex_(-1)
	, renderCommandQueued_(false)
	, pendingFrameReadyTime_(std::chrono::steady_clock::time_point::min())
	, initialized_(false)
	, destroyed_(false)
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

	BeginRenderCommand();
	ENQUEUE_RENDER_COMMAND(InitializeReader)(
		[this, texture](FRHICommandListImmediate& RHICmdList)
		{
			std::lock_guard<std::mutex> resourceLock(resourceMutex_);

			if (destroyed_)
			{
				EndRenderCommand();
				return;
			}

			texture_ = texture->GetResource()->GetTexture2DRHI();
			frameWidth_ = texture_->GetDesc().Extent.X;
			frameHeight_ = texture_->GetDesc().Extent.Y;

			const size_t bufferSize = static_cast<size_t>(frameWidth_) * static_cast<size_t>(frameHeight_) * static_cast<size_t>(bytesPerPixel_);
			frameBuffers_[0] = new uint8_t[bufferSize];
			frameBuffers_[1] = new uint8_t[bufferSize];
			initialized_ = true;

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

	delete[] frameBuffers_[0];
	delete[] frameBuffers_[1];
	frameBuffers_[0] = nullptr;
	frameBuffers_[1] = nullptr;

	texture_ = nullptr;
}

void RenderTargetWriter::Process()
{
	const uint8_t* inputData = GetInputPin<0>().GetData();
	const uint32_t inputSize = GetInputPin<0>().GetSize();

	if (!initialized_ || !inputData || inputSize != frameWidth_ * frameHeight_ * bytesPerPixel_)
	{
		return;
	}

	double copyMs = 0.0;
	bool droppedPendingFrame = false;
	bool shouldQueueRenderCommand = false;

	{
		std::lock_guard<std::mutex> writeLock(writeMutex_);

		uint8_t targetBufferIndex = pendingFrameAvailable_ ? pendingBufferIndex_ : 0;
		if (uploadBufferIndex_ >= 0 && targetBufferIndex == static_cast<uint8_t>(uploadBufferIndex_))
		{
			targetBufferIndex = static_cast<uint8_t>(1 - uploadBufferIndex_);
		}

		droppedPendingFrame = pendingFrameAvailable_ && targetBufferIndex == pendingBufferIndex_;

		const auto copyStart = std::chrono::steady_clock::now();
		std::memcpy(frameBuffers_[targetBufferIndex], inputData, inputSize);
		const auto copyEnd = std::chrono::steady_clock::now();
		copyMs = std::chrono::duration<double, std::milli>(copyEnd - copyStart).count();

		pendingBufferIndex_ = targetBufferIndex;
		pendingFrameAvailable_ = true;
		pendingFrameReadyTime_ = copyEnd;

		if (!renderCommandQueued_)
		{
			renderCommandQueued_ = true;
			shouldQueueRenderCommand = true;
		}
	}

	RecordCopySample(copyMs);
	if (droppedPendingFrame)
	{
		RecordDroppedFrame();
	}

	if (shouldQueueRenderCommand)
	{
		QueueUploadRenderCommand();
	}
}

void RenderTargetWriter::QueueUploadRenderCommand()
{
	BeginRenderCommand();
	ENQUEUE_RENDER_COMMAND(ExtractRenderTargets)(
		[this](FRHICommandListImmediate& RHICmdList)
		{
			std::unique_lock<std::mutex> resourceLock(resourceMutex_);

			if (!initialized_ || destroyed_)
			{
				{
					std::lock_guard<std::mutex> writeLock(writeMutex_);
					renderCommandQueued_ = false;
					pendingFrameAvailable_ = false;
					uploadBufferIndex_ = -1;
				}

				resourceLock.unlock();
				EndRenderCommand();
				return;
			}

			FUpdateTextureRegion2D updateRegion = {};
			updateRegion.SrcX = 0;
			updateRegion.SrcY = 0;
			updateRegion.DestX = 0;
			updateRegion.DestY = 0;
			updateRegion.Width = frameWidth_;
			updateRegion.Height = frameHeight_;

			const uint8_t* uploadBuffer = nullptr;
			double queueWaitMs = 0.0;
			double updateMs = 0.0;
			double endToEndMs = 0.0;
			bool shouldQueueAnother = false;
			std::chrono::steady_clock::time_point frameReadyTime = std::chrono::steady_clock::time_point::min();

			{
				std::lock_guard<std::mutex> writeLock(writeMutex_);

				if (!pendingFrameAvailable_)
				{
					renderCommandQueued_ = false;
					resourceLock.unlock();
					EndRenderCommand();
					return;
				}

				uploadBufferIndex_ = static_cast<int8_t>(pendingBufferIndex_);
				pendingFrameAvailable_ = false;
				frameReadyTime = pendingFrameReadyTime_;
				uploadBuffer = frameBuffers_[uploadBufferIndex_];
			}

			const auto renderStart = std::chrono::steady_clock::now();
			queueWaitMs = std::chrono::duration<double, std::milli>(renderStart - frameReadyTime).count();

			const auto updateStart = renderStart;
			RHICmdList.UpdateTexture2D(texture_, 0, updateRegion, frameWidth_ * bytesPerPixel_, uploadBuffer);
			const auto updateEnd = std::chrono::steady_clock::now();
			updateMs = std::chrono::duration<double, std::milli>(updateEnd - updateStart).count();
			endToEndMs = std::chrono::duration<double, std::milli>(updateEnd - frameReadyTime).count();

			{
				std::lock_guard<std::mutex> writeLock(writeMutex_);
				uploadBufferIndex_ = -1;
				if (pendingFrameAvailable_)
				{
					shouldQueueAnother = true;
				}
				else
				{
					renderCommandQueued_ = false;
				}
			}

			resourceLock.unlock();
			EndRenderCommand();
			RecordRenderSamples(queueWaitMs, updateMs, endToEndMs);

			if (shouldQueueAnother)
			{
				QueueUploadRenderCommand();
			}
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
