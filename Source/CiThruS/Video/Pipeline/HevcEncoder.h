#pragma once

#if __has_include("Kvazaar/Include/kvazaar.h")
#ifndef CITHRUS_KVAZAAR_AVAILABLE
#define CITHRUS_KVAZAAR_AVAILABLE
#endif // CITHRUS_KVAZAAR_AVAILABLE
#include "Kvazaar/Include/kvazaar.h"
#else
#pragma message (__FILE__ ": warning: Kvazaar not found, HEVC encoding is unavailable")
#endif // __has_include(...)

#if defined(CITHRUS_VIDEOTOOLBOX_AVAILABLE)
#include <CoreMedia/CoreMedia.h>
#include <CoreVideo/CoreVideo.h>
#include <VideoToolbox/VideoToolbox.h>

#include <condition_variable>
#include <deque>
#include <mutex>
#include <vector>
#endif // CITHRUS_VIDEOTOOLBOX_AVAILABLE

#include "CoreMinimal.h"
#include "PipelineFilter.h"
#include "Video/HevcEncoderBackend.h"

#include <chrono>
#include <string>
#include <vector>

enum HevcEncoderPreset : uint8_t
{
	HevcPresetNone,
	HevcPresetMinimumLatency,
	HevcPresetLossless
};

class CITHRUS_API HevcEncoder : public PipelineFilter<1, 1>
{
public:
	HevcEncoder(
		const uint16_t& frameWidth,
		const uint16_t& frameHeight,
		const uint8_t& threadCount,
		const uint8_t& qp,
		const uint8_t& wpp,
		const uint8_t& owf,
		const HevcEncoderPreset& preset = HevcPresetNone,
		const EHevcEncoderBackend& requestedBackend = EHevcEncoderBackend::Auto,
		const float& targetBitrateMbps = 1.5f,
		const uint32_t& maxKeyFrameInterval = 60,
		const uint32_t& expectedFrameRate = 60,
		const uint32_t& perfLogInterval = 120,
		const uint32_t& maxPendingFrames = 3);
	virtual ~HevcEncoder();

	virtual void Process() override;

	static EHevcEncoderBackend ResolveBackend(const EHevcEncoderBackend& requestedBackend);
	inline EHevcEncoderBackend GetResolvedBackend() const { return selectedBackend_; }

protected:
	uint32_t frameWidth_;
	uint32_t frameHeight_;

	uint8_t* outputData_;

	std::chrono::high_resolution_clock::time_point startTime_;
	EHevcEncoderBackend requestedBackend_;
	EHevcEncoderBackend selectedBackend_;
	HevcEncoderPreset preset_;
	uint8_t threadCount_;
	uint8_t qp_;
	uint8_t wpp_;
	uint8_t owf_;
	float targetBitrateMbps_;
	uint32_t maxKeyFrameInterval_;
	uint32_t expectedFrameRate_;
	uint32_t perfLogInterval_;
	uint32_t maxPendingFrames_;
	uint32_t perfFramesAccumulated_;
	uint64_t droppedFrames_;
	std::vector<uint8_t> kvazaarScratchBuffer_;

	void PublishOutputFrame(const std::vector<uint8_t>& frameData);
	void ClearOutputFrame();
	void LogPerformanceIfNeeded();
	FString BackendName() const;

#ifdef CITHRUS_KVAZAAR_AVAILABLE
	const kvz_api* kvazaarApi_ = kvz_api_get(8);
	kvz_config* kvazaarConfig_ = nullptr;
	kvz_encoder* kvazaarEncoder_ = nullptr;
	kvz_picture* kvazaarTransmitPicture_ = nullptr;

	bool InitializeKvazaar();
	void ShutdownKvazaar();
	bool EncodeWithKvazaar(const uint8_t* inputData, uint32_t inputSize, const std::string& inputFormat);
#endif // CITHRUS_KVAZAAR_AVAILABLE

#ifdef CITHRUS_VIDEOTOOLBOX_AVAILABLE
	struct EncodedFrame
	{
		std::vector<uint8_t> Data;
		double EncodeMilliseconds = 0.0;
		bool Keyframe = false;
	};

	struct PendingFrameContext
	{
		HevcEncoder* Encoder = nullptr;
		int64_t FrameIndex = 0;
		std::chrono::high_resolution_clock::time_point SubmitTime;
		bool ForceKeyframe = false;
	};

	struct VideoToolboxCallbackState
	{
		std::mutex Mutex;
		std::condition_variable Cv;
		HevcEncoder* Encoder = nullptr;
		uint32_t ActiveCallbacks = 0;
	};

	VTCompressionSessionRef compressionSession_ = nullptr;
	CVPixelBufferPoolRef pixelBufferPool_ = nullptr;
	CFDictionaryRef vtPixelBufferAttributes_ = nullptr;
	VideoToolboxCallbackState* callbackState_ = nullptr;
	bool ownPixelBufferPool_ = false;
	bool useVideoToolbox_ = false;
	bool usingHardwareVideoToolbox_ = false;
	OSType vtPixelFormat_ = kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange;
	int64_t frameCounter_ = 0;
	std::mutex encodeMutex_;
	std::deque<EncodedFrame> completedFrames_;
	uint32_t pendingEncodeCount_ = 0;
	uint32_t droppedCompletedFrames_ = 0;
	uint64_t vtAllocSamples_ = 0;
	uint64_t vtConvertSamples_ = 0;
	uint64_t vtEncodeSamples_ = 0;
	double vtAllocMsTotal_ = 0.0;
	double vtConvertMsTotal_ = 0.0;
	double vtEncodeMsTotal_ = 0.0;

	static bool SupportsVideoToolbox();
	static bool IsHardwareVideoToolboxAvailable();
	bool InitializeVideoToolbox(bool requireHardware);
	void ShutdownVideoToolbox();
	bool CreateFallbackPixelBufferPool();
	bool EnsureVideoToolboxPixelBufferPool();
	bool EncodeWithVideoToolbox(const uint8_t* inputData, uint32_t inputSize, const std::string& inputFormat);
	bool ConvertInputToPixelBuffer(const uint8_t* inputData, uint32_t inputSize, const std::string& inputFormat, CVPixelBufferRef pixelBuffer);
	void DrainCompletedVideoToolboxFrames();
	void HandleEncodedFrame(OSStatus status, CMSampleBufferRef sampleBuffer, PendingFrameContext* frameContext);

	static void CompressionCallback(
		void* outputCallbackRefCon,
		void* sourceFrameRefCon,
		OSStatus status,
		VTEncodeInfoFlags infoFlags,
		CMSampleBufferRef sampleBuffer);
#endif // CITHRUS_VIDEOTOOLBOX_AVAILABLE
};
