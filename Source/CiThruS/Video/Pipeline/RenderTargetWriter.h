#pragma once

#include "RHIResources.h"
#include "PipelineSink.h"

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <mutex>

class UTexture2D;
class UTextureRenderTarget2D;

// Writes data into RHI render targets in VRAM
class CITHRUS_API RenderTargetWriter : public PipelineSink<1>
{
public:
	RenderTargetWriter(UTextureRenderTarget2D* texture);
	~RenderTargetWriter();

	virtual void Process() override;

protected:
	uint8_t* frameBuffers_[2];

	FRHITexture* texture_;

	uint16_t frameWidth_;
	uint16_t frameHeight_;
	uint8_t bytesPerPixel_;

	// Latest frame waiting to be uploaded by the render thread
	bool pendingFrameAvailable_;
	uint8_t pendingBufferIndex_;
	int8_t uploadBufferIndex_;
	bool renderCommandQueued_;
	std::chrono::steady_clock::time_point pendingFrameReadyTime_;

	// Used to prevent the worker thread and render thread from touching the same CPU buffer at once
	std::mutex writeMutex_;

	// Used to prevent resources from being deleted while they might still be in use on another thread
	std::mutex resourceMutex_;
	std::mutex renderCommandMutex_;
	std::condition_variable renderCommandCv_;
	uint32_t pendingRenderCommandCount_ = 0;

	std::mutex perfMutex_;
	double uploadCopyMsTotal_ = 0.0;
	double uploadQueueWaitMsTotal_ = 0.0;
	double uploadUpdateMsTotal_ = 0.0;
	double uploadEndToEndMsTotal_ = 0.0;
	uint32_t uploadCopySamples_ = 0;
	uint32_t uploadQueueWaitSamples_ = 0;
	uint32_t uploadUpdateSamples_ = 0;
	uint32_t uploadEndToEndSamples_ = 0;
	uint32_t perfFramesAccumulated_ = 0;
	uint32_t perfLogInterval_ = 120;
	uint64_t droppedFrames_ = 0;

	// These are separate because theoretically this object may get destroyed before being initialized,
	// in which case the initialization needs to be cancelled
	bool initialized_;
	bool destroyed_;

	void QueueUploadRenderCommand();
	void BeginRenderCommand();
	void EndRenderCommand();
	void RecordCopySample(double milliseconds);
	void RecordDroppedFrame(uint64_t count = 1);
	void RecordRenderSamples(double queueWaitMs, double updateMs, double endToEndMs);
};
