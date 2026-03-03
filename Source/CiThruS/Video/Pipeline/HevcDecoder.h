#pragma once

#include "CoreMinimal.h"

#if (!defined(CITHRUS_OPENHEVC_DISABLED) || CITHRUS_OPENHEVC_DISABLED == 0) && __has_include("OpenHEVC/Include/openHevcWrapper.h")
#ifndef CITHRUS_OPENHEVC_AVAILABLE
#define CITHRUS_OPENHEVC_AVAILABLE
#endif // CITHRUS_OPENHEVC_AVAILABLE
#include "OpenHEVC/Include/openHevcWrapper.h"
#else
#pragma message (__FILE__ ": warning: OpenHEVC not found or disabled, HEVC decoding is unavailable")
#endif // guard + __has_include(...)

#ifdef CITHRUS_VIDEOTOOLBOX_AVAILABLE
#include <CoreMedia/CoreMedia.h>
#include <CoreVideo/CoreVideo.h>
#include <VideoToolbox/VideoToolbox.h>
#endif // CITHRUS_VIDEOTOOLBOX_AVAILABLE

#include "PipelineFilter.h"

#include <vector>

enum class EHevcDecoderBackend : uint8;

// Decodes HEVC video into YUV 4:2:0 data
class CITHRUS_API HevcDecoder : public PipelineFilter<1, 1>
{
public:
	HevcDecoder(const uint8_t& threadCount);
	HevcDecoder(const uint8_t& threadCount, EHevcDecoderBackend backend);
	virtual ~HevcDecoder();

	virtual void Process() override;

protected:
	void SelectInitialBackend();
	void DisableAllBackends(const char* reason);
	void TryFallbackToOpenHevc(const char* reason);

	bool DecodeWithOpenHevc(const uint8_t* inputData, uint32_t inputSize, bool hasAnnexBPrefix);
	bool InitializeOpenHevc();
	void ShutdownOpenHevc();

	uint8_t* outputData_;
	uint32_t outputSize_;

	uint32_t bufferSizes_[2];
	uint8_t* buffers_[2];

	uint8_t bufferIndex_;
	uint8_t threadCount_;

	EHevcDecoderBackend requestedBackend_;
	bool useVideoToolbox_;
	bool useOpenHevc_;
	bool backendConfigured_;
	uint32_t openHevcErrorCount_;
	uint32_t videoToolboxErrorCount_;

#ifdef CITHRUS_OPENHEVC_AVAILABLE
	OpenHevc_Handle handle_;
	std::vector<uint8_t> openHevcAnnexBPacket_;
#endif // CITHRUS_OPENHEVC_AVAILABLE

#ifdef CITHRUS_VIDEOTOOLBOX_AVAILABLE
	bool EnsureVideoToolboxSession();
	bool CreateOrRecreateVideoToolboxSession();
	void DestroyVideoToolboxSession();
	bool CacheParameterSet(const uint8_t* nalData, uint32_t nalSize, uint8_t nalType);
	bool DecodeWithVideoToolboxSample(const uint8_t* sampleData, uint32_t sampleSize);
	bool DecodeWithVideoToolboxNal(const uint8_t* nalData, uint32_t nalSize);
	void HandleDecodedFrame(OSStatus status, CVImageBufferRef imageBuffer);

	static void DecompressionOutputCallback(
		void* decompressionOutputRefCon,
		void* sourceFrameRefCon,
		OSStatus status,
		VTDecodeInfoFlags infoFlags,
		CVImageBufferRef imageBuffer,
		CMTime presentationTimeStamp,
		CMTime presentationDuration);

	VTDecompressionSessionRef videoToolboxSession_;
	CMVideoFormatDescriptionRef videoToolboxFormatDescription_;

	std::vector<uint8_t> vps_;
	std::vector<uint8_t> sps_;
	std::vector<uint8_t> pps_;
	bool videoToolboxParameterSetsDirty_;
	bool videoToolboxDecodedFrameReady_;

	std::vector<uint8_t> videoToolboxPendingAuHvcc_;
	bool videoToolboxPendingAuHasVcl_;
	uint32_t videoToolboxPendingAuNalCount_;
	uint32_t videoToolboxSubmittedSampleCount_;
	uint32_t videoToolboxDecodedSampleCount_;
	uint32_t videoToolboxNoOutputSampleCount_;
	uint32_t videoToolboxNalLogCount_;
	uint32_t videoToolboxWaitingForParamsDropCount_;
#endif // CITHRUS_VIDEOTOOLBOX_AVAILABLE
};
