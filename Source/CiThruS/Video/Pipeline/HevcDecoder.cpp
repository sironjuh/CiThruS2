#include "HevcDecoder.h"

#include "Misc/Debug.h"
#include "ViewSynthesis/ViewSynthesizer.h"

#include <cstring>
#include <vector>

namespace
{
struct FNalUnitView
{
	const uint8_t* data = nullptr;
	uint32_t size = 0;
	uint8_t type = 0xFF;
};

constexpr uint8_t ANNEX_B_START_CODE[4] = { 0x00, 0x00, 0x00, 0x01 };

bool StartsWithAnnexB(const uint8_t* data, uint32_t size)
{
	if (!data || size < 3)
	{
		return false;
	}

	if (size >= 4 && data[0] == 0x00 && data[1] == 0x00 && data[2] == 0x00 && data[3] == 0x01)
	{
		return true;
	}

	return data[0] == 0x00 && data[1] == 0x00 && data[2] == 0x01;
}

int32_t FindNextStartCode(const uint8_t* data, uint32_t startOffset, uint32_t endOffset)
{
	if (!data || startOffset >= endOffset || endOffset < 3)
	{
		return -1;
	}

	for (uint32_t i = startOffset; i + 2 < endOffset; ++i)
	{
		if (data[i] == 0x00 && data[i + 1] == 0x00)
		{
			if (data[i + 2] == 0x01)
			{
				return static_cast<int32_t>(i);
			}

			if (i + 3 < endOffset && data[i + 2] == 0x00 && data[i + 3] == 0x01)
			{
				return static_cast<int32_t>(i);
			}
		}
	}

	return -1;
}

uint8_t GetHevcNalType(const uint8_t* nalData, uint32_t nalSize)
{
	if (!nalData || nalSize == 0)
	{
		return 0xFF;
	}

	return (nalData[0] & 0x7E) >> 1;
}

bool IsParameterSetNal(const uint8_t nalType)
{
	return nalType == 32 || nalType == 33 || nalType == 34;
}

bool IsVclNal(const uint8_t nalType)
{
	return nalType <= 31;
}

bool IsFirstSliceSegmentInPicture(const uint8_t* nalData, const uint32_t nalSize, const uint8_t nalType)
{
	if (!IsVclNal(nalType) || !nalData || nalSize < 3)
	{
		return false;
	}

	// HEVC slice_segment_header starts immediately after 2-byte NAL header.
	// first_slice_segment_in_pic_flag is the top bit of the first slice header byte.
	return (nalData[2] & 0x80) != 0;
}

#ifdef CITHRUS_VIDEOTOOLBOX_AVAILABLE
const TCHAR* DecodeVtStatus(const OSStatus status)
{
	switch (status)
	{
	case noErr:
		return TEXT("noErr");
	case kVTVideoDecoderBadDataErr:
		return TEXT("kVTVideoDecoderBadDataErr");
	case kVTVideoDecoderUnsupportedDataFormatErr:
		return TEXT("kVTVideoDecoderUnsupportedDataFormatErr");
	case kVTVideoDecoderMalfunctionErr:
		return TEXT("kVTVideoDecoderMalfunctionErr");
	case kVTInvalidSessionErr:
		return TEXT("kVTInvalidSessionErr");
	default:
		return TEXT("unknown");
	}
}
#endif // CITHRUS_VIDEOTOOLBOX_AVAILABLE

void ExtractNalUnits(const uint8_t* data, uint32_t size, const bool hasAnnexBPrefix, std::vector<FNalUnitView>& nalUnits)
{
	nalUnits.clear();

	if (!data || size == 0)
	{
		return;
	}

	if (!hasAnnexBPrefix)
	{
		nalUnits.push_back(FNalUnitView{ data, size, GetHevcNalType(data, size) });
		return;
	}

	uint32_t offset = 0;
	while (offset + 3 <= size)
	{
		uint32_t startCodeLength = 0;
		if (offset + 3 <= size && data[offset] == 0x00 && data[offset + 1] == 0x00 && data[offset + 2] == 0x01)
		{
			startCodeLength = 3;
		}
		else if (offset + 4 <= size && data[offset] == 0x00 && data[offset + 1] == 0x00 &&
				 data[offset + 2] == 0x00 && data[offset + 3] == 0x01)
		{
			startCodeLength = 4;
		}
		else
		{
			const int32_t nextStartCode = FindNextStartCode(data, offset + 1, size);
			if (nextStartCode < 0)
			{
				break;
			}

			offset = static_cast<uint32_t>(nextStartCode);
			continue;
		}

		const uint32_t nalStart = offset + startCodeLength;
		if (nalStart >= size)
		{
			break;
		}

		const int32_t nextStartCode = FindNextStartCode(data, nalStart, size);
		const uint32_t nalEnd = nextStartCode >= 0 ? static_cast<uint32_t>(nextStartCode) : size;
		const uint32_t nalSize = nalEnd > nalStart ? nalEnd - nalStart : 0;

		if (nalSize > 0)
		{
			nalUnits.push_back(FNalUnitView{ data + nalStart, nalSize, GetHevcNalType(data + nalStart, nalSize) });
		}

		offset = nalEnd;
	}
}
} // namespace

HevcDecoder::HevcDecoder(const uint8_t& threadCount)
	: HevcDecoder(threadCount, EHevcDecoderBackend::Auto)
{
}

HevcDecoder::HevcDecoder(const uint8_t& threadCount, EHevcDecoderBackend backend)
	: outputData_(nullptr)
	, outputSize_(0)
	, bufferIndex_(0)
	, threadCount_(threadCount)
	, requestedBackend_(backend)
	, useVideoToolbox_(false)
	, useOpenHevc_(false)
	, backendConfigured_(false)
	, openHevcErrorCount_(0)
	, videoToolboxErrorCount_(0)
#ifdef CITHRUS_OPENHEVC_AVAILABLE
	, handle_(nullptr)
#endif // CITHRUS_OPENHEVC_AVAILABLE
#ifdef CITHRUS_VIDEOTOOLBOX_AVAILABLE
	, videoToolboxSession_(nullptr)
	, videoToolboxFormatDescription_(nullptr)
	, videoToolboxParameterSetsDirty_(false)
	, videoToolboxDecodedFrameReady_(false)
	, videoToolboxPendingAuHasVcl_(false)
	, videoToolboxPendingAuNalCount_(0)
	, videoToolboxSubmittedSampleCount_(0)
	, videoToolboxDecodedSampleCount_(0)
	, videoToolboxNoOutputSampleCount_(0)
	, videoToolboxNalLogCount_(0)
#endif // CITHRUS_VIDEOTOOLBOX_AVAILABLE
{
	buffers_[0] = nullptr;
	buffers_[1] = nullptr;

	bufferSizes_[0] = 0;
	bufferSizes_[1] = 0;

	GetInputPin<0>().SetAcceptedFormat("hevc");
	GetOutputPin<0>().SetFormat("yuv420");

	SelectInitialBackend();
}

HevcDecoder::~HevcDecoder()
{
#ifdef CITHRUS_VIDEOTOOLBOX_AVAILABLE
	DestroyVideoToolboxSession();
#endif // CITHRUS_VIDEOTOOLBOX_AVAILABLE

	ShutdownOpenHevc();

	delete[] buffers_[0];
	delete[] buffers_[1];

	buffers_[0] = nullptr;
	buffers_[1] = nullptr;
	bufferSizes_[0] = 0;
	bufferSizes_[1] = 0;

	outputData_ = nullptr;
	outputSize_ = 0;
	GetOutputPin<0>().SetData(nullptr);
	GetOutputPin<0>().SetSize(0);
}

void HevcDecoder::SelectInitialBackend()
{
#if defined(CITHRUS_VIDEOTOOLBOX_AVAILABLE) && PLATFORM_MAC
	const bool canUseVideoToolbox = true;
#else
	const bool canUseVideoToolbox = false;
#endif

	if (requestedBackend_ == EHevcDecoderBackend::VideoToolbox ||
		(requestedBackend_ == EHevcDecoderBackend::Auto && canUseVideoToolbox))
	{
		if (canUseVideoToolbox)
		{
			useVideoToolbox_ = true;
			backendConfigured_ = true;
			UE_LOG(LogTemp, Log, TEXT("HevcDecoder: Using VideoToolbox backend%s"),
				requestedBackend_ == EHevcDecoderBackend::Auto ? TEXT(" (Auto mode)") : TEXT(""));
			return;
		}

		UE_LOG(LogTemp, Warning, TEXT("HevcDecoder: VideoToolbox requested but unavailable in this build, trying OpenHEVC"));
	}

	if (InitializeOpenHevc())
	{
		useOpenHevc_ = true;
		backendConfigured_ = true;
		UE_LOG(LogTemp, Log, TEXT("HevcDecoder: Using OpenHEVC backend"));
		return;
	}

	DisableAllBackends("no HEVC decoder backend available");
}

void HevcDecoder::DisableAllBackends(const char* reason)
{
	useVideoToolbox_ = false;
	useOpenHevc_ = false;
	backendConfigured_ = false;

#ifdef CITHRUS_VIDEOTOOLBOX_AVAILABLE
	DestroyVideoToolboxSession();
#endif // CITHRUS_VIDEOTOOLBOX_AVAILABLE

	ShutdownOpenHevc();

	UE_LOG(LogTemp, Warning, TEXT("HevcDecoder: decoding disabled (%hs)"), reason ? reason : "unknown reason");
}

void HevcDecoder::TryFallbackToOpenHevc(const char* reason)
{
	if (useOpenHevc_)
	{
		return;
	}

	if (InitializeOpenHevc())
	{
		useOpenHevc_ = true;
		useVideoToolbox_ = false;
		backendConfigured_ = true;

#ifdef CITHRUS_VIDEOTOOLBOX_AVAILABLE
		DestroyVideoToolboxSession();
#endif // CITHRUS_VIDEOTOOLBOX_AVAILABLE

		UE_LOG(LogTemp, Warning, TEXT("HevcDecoder: falling back to OpenHEVC (%hs)"), reason ? reason : "unknown reason");
		return;
	}

	DisableAllBackends(reason ? reason : "fallback to OpenHEVC failed");
}

bool HevcDecoder::InitializeOpenHevc()
{
#ifdef CITHRUS_OPENHEVC_AVAILABLE
	if (handle_)
	{
		return true;
	}

	handle_ = libOpenHevcInit(static_cast<int>(threadCount_), 2);
	if (!handle_)
	{
		UE_LOG(LogTemp, Warning, TEXT("HevcDecoder: libOpenHevcInit failed"));
		return false;
	}

	if (libOpenHevcStartDecoder(handle_) == -1)
	{
		UE_LOG(LogTemp, Warning, TEXT("HevcDecoder: libOpenHevcStartDecoder failed"));
		libOpenHevcClose(handle_);
		handle_ = nullptr;
		return false;
	}

	libOpenHevcSetTemporalLayer_id(handle_, 0);
	libOpenHevcSetActiveDecoders(handle_, 0);
	libOpenHevcSetViewLayers(handle_, 0);
	return true;
#else
	return false;
#endif // CITHRUS_OPENHEVC_AVAILABLE
}

void HevcDecoder::ShutdownOpenHevc()
{
#ifdef CITHRUS_OPENHEVC_AVAILABLE
	if (handle_)
	{
		libOpenHevcClose(handle_);
		handle_ = nullptr;
	}

	openHevcAnnexBPacket_.clear();
#endif // CITHRUS_OPENHEVC_AVAILABLE
}

bool HevcDecoder::DecodeWithOpenHevc(const uint8_t* inputData, uint32_t inputSize, bool hasAnnexBPrefix)
{
#ifdef CITHRUS_OPENHEVC_AVAILABLE
	if (!InitializeOpenHevc())
	{
		return false;
	}

	const uint8_t* decodeData = inputData;
	uint32_t decodeSize = inputSize;

	if (!hasAnnexBPrefix)
	{
		openHevcAnnexBPacket_.resize(inputSize + sizeof(ANNEX_B_START_CODE));
		std::memcpy(openHevcAnnexBPacket_.data(), ANNEX_B_START_CODE, sizeof(ANNEX_B_START_CODE));
		std::memcpy(openHevcAnnexBPacket_.data() + sizeof(ANNEX_B_START_CODE), inputData, inputSize);

		decodeData = openHevcAnnexBPacket_.data();
		decodeSize = static_cast<uint32_t>(openHevcAnnexBPacket_.size());
	}

	OpenHevc_Frame decodedFrame;
	const int decodeStatus = libOpenHevcDecode(handle_, decodeData, static_cast<int>(decodeSize), 0);
	if (decodeStatus < 0)
	{
		++openHevcErrorCount_;
		if (openHevcErrorCount_ <= 5 || (openHevcErrorCount_ % 60) == 0)
		{
			UE_LOG(LogTemp, Warning, TEXT("HevcDecoder(OpenHEVC): libOpenHevcDecode failed (status=%d)"), decodeStatus);
		}
		return false;
	}

	const int outputStatus = libOpenHevcGetOutput(handle_, decodeStatus, &decodedFrame);
	if (outputStatus == 0)
	{
		return false;
	}

	libOpenHevcGetPictureInfo(handle_, &decodedFrame.frameInfo);
	const int width = decodedFrame.frameInfo.nWidth;
	const int height = decodedFrame.frameInfo.nHeight;

	if (width <= 0 || height <= 0)
	{
		++openHevcErrorCount_;
		UE_LOG(LogTemp, Warning, TEXT("HevcDecoder(OpenHEVC): invalid decoded frame size %dx%d"), width, height);
		return false;
	}

	outputSize_ = static_cast<uint32_t>(width * height * 3 / 2);
	if (bufferSizes_[bufferIndex_] != outputSize_)
	{
		delete[] buffers_[bufferIndex_];
		buffers_[bufferIndex_] = new uint8_t[outputSize_];
		bufferSizes_[bufferIndex_] = outputSize_;
	}

	// Copy Y
	for (int row = 0; row < height; ++row)
	{
		std::memcpy(
			buffers_[bufferIndex_] + row * width,
			reinterpret_cast<uint8_t*>(decodedFrame.pvY) + row * decodedFrame.frameInfo.nYPitch,
			width);
	}

	// Copy U and V
	for (int row = 0; row < height / 2; ++row)
	{
		std::memcpy(
			buffers_[bufferIndex_] + width * height + row * width / 2,
			reinterpret_cast<uint8_t*>(decodedFrame.pvU) + row * decodedFrame.frameInfo.nUPitch,
			width / 2);
		std::memcpy(
			buffers_[bufferIndex_] + width * height * 5 / 4 + row * width / 2,
			reinterpret_cast<uint8_t*>(decodedFrame.pvV) + row * decodedFrame.frameInfo.nVPitch,
			width / 2);
	}

	outputData_ = buffers_[bufferIndex_];
	return true;
#else
	(void)inputData;
	(void)inputSize;
	(void)hasAnnexBPrefix;
	return false;
#endif // CITHRUS_OPENHEVC_AVAILABLE
}

#ifdef CITHRUS_VIDEOTOOLBOX_AVAILABLE
bool HevcDecoder::CacheParameterSet(const uint8_t* nalData, uint32_t nalSize, uint8_t nalType)
{
	std::vector<uint8_t>* destination = nullptr;

	switch (nalType)
	{
	case 32:
		destination = &vps_;
		break;
	case 33:
		destination = &sps_;
		break;
	case 34:
		destination = &pps_;
		break;
	default:
		return false;
	}

	if (destination->size() == nalSize &&
		(nalSize == 0 || std::memcmp(destination->data(), nalData, nalSize) == 0))
	{
		return false;
	}

	destination->assign(nalData, nalData + nalSize);
	videoToolboxParameterSetsDirty_ = true;
	return true;
}

bool HevcDecoder::EnsureVideoToolboxSession()
{
	if (!useVideoToolbox_)
	{
		return false;
	}

	if (vps_.empty() || sps_.empty() || pps_.empty())
	{
		return false;
	}

	if (!videoToolboxSession_ || !videoToolboxFormatDescription_ || videoToolboxParameterSetsDirty_)
	{
		return CreateOrRecreateVideoToolboxSession();
	}

	return true;
}

bool HevcDecoder::CreateOrRecreateVideoToolboxSession()
{
	if (vps_.empty() || sps_.empty() || pps_.empty())
	{
		return false;
	}

	DestroyVideoToolboxSession();

	const uint8_t* parameterSetPointers[] = { vps_.data(), sps_.data(), pps_.data() };
	const size_t parameterSetSizes[] = { vps_.size(), sps_.size(), pps_.size() };
	const size_t parameterSetCount = 3;

	OSStatus status = CMVideoFormatDescriptionCreateFromHEVCParameterSets(
		kCFAllocatorDefault,
		parameterSetCount,
		parameterSetPointers,
		parameterSetSizes,
		4,
		nullptr,
		&videoToolboxFormatDescription_);

	if (status != noErr || !videoToolboxFormatDescription_)
	{
		++videoToolboxErrorCount_;
		UE_LOG(LogTemp, Warning, TEXT("HevcDecoder(VideoToolbox): CMVideoFormatDescriptionCreateFromHEVCParameterSets failed status=%d"), (int)status);
		return false;
	}

	CFMutableDictionaryRef destinationAttributes = CFDictionaryCreateMutable(
		kCFAllocatorDefault,
		2,
		&kCFTypeDictionaryKeyCallBacks,
		&kCFTypeDictionaryValueCallBacks);

	const OSType pixelFormat = kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange;
	CFNumberRef pixelFormatNumber = CFNumberCreate(kCFAllocatorDefault, kCFNumberSInt32Type, &pixelFormat);
	CFDictionarySetValue(destinationAttributes, kCVPixelBufferPixelFormatTypeKey, pixelFormatNumber);
	CFRelease(pixelFormatNumber);

	CFMutableDictionaryRef ioSurfaceProperties = CFDictionaryCreateMutable(
		kCFAllocatorDefault,
		0,
		&kCFTypeDictionaryKeyCallBacks,
		&kCFTypeDictionaryValueCallBacks);
	CFDictionarySetValue(destinationAttributes, kCVPixelBufferIOSurfacePropertiesKey, ioSurfaceProperties);
	CFRelease(ioSurfaceProperties);

	VTDecompressionOutputCallbackRecord callbackRecord = {};
	callbackRecord.decompressionOutputRefCon = this;
	callbackRecord.decompressionOutputCallback = DecompressionOutputCallback;

	status = VTDecompressionSessionCreate(
		kCFAllocatorDefault,
		videoToolboxFormatDescription_,
		nullptr,
		destinationAttributes,
		&callbackRecord,
		&videoToolboxSession_);

	CFRelease(destinationAttributes);

	if (status != noErr || !videoToolboxSession_)
	{
		++videoToolboxErrorCount_;
		UE_LOG(LogTemp, Warning, TEXT("HevcDecoder(VideoToolbox): VTDecompressionSessionCreate failed status=%d"), (int)status);
		DestroyVideoToolboxSession();
		return false;
	}

	videoToolboxParameterSetsDirty_ = false;
	UE_LOG(LogTemp, Log, TEXT("HevcDecoder(VideoToolbox): decoder session configured"));
	return true;
}

void HevcDecoder::DestroyVideoToolboxSession()
{
	if (videoToolboxSession_)
	{
		VTDecompressionSessionInvalidate(videoToolboxSession_);
		CFRelease(videoToolboxSession_);
		videoToolboxSession_ = nullptr;
	}

	if (videoToolboxFormatDescription_)
	{
		CFRelease(videoToolboxFormatDescription_);
		videoToolboxFormatDescription_ = nullptr;
	}
}

bool HevcDecoder::DecodeWithVideoToolboxNal(const uint8_t* nalData, uint32_t nalSize)
{
	if (!nalData || nalSize == 0)
	{
		return false;
	}

	std::vector<uint8_t> hvccSample(4 + nalSize);
	hvccSample[0] = static_cast<uint8_t>((nalSize >> 24) & 0xFF);
	hvccSample[1] = static_cast<uint8_t>((nalSize >> 16) & 0xFF);
	hvccSample[2] = static_cast<uint8_t>((nalSize >> 8) & 0xFF);
	hvccSample[3] = static_cast<uint8_t>(nalSize & 0xFF);
	std::memcpy(hvccSample.data() + 4, nalData, nalSize);

	return DecodeWithVideoToolboxSample(hvccSample.data(), static_cast<uint32_t>(hvccSample.size()));
}

bool HevcDecoder::DecodeWithVideoToolboxSample(const uint8_t* sampleData, uint32_t sampleSize)
{
	if (!videoToolboxSession_ || !videoToolboxFormatDescription_ || !sampleData || sampleSize == 0)
	{
		return false;
	}

	CMBlockBufferRef blockBuffer = nullptr;
	OSStatus status = CMBlockBufferCreateWithMemoryBlock(
		kCFAllocatorDefault,
		nullptr,
		sampleSize,
		kCFAllocatorDefault,
		nullptr,
		0,
		sampleSize,
		0,
		&blockBuffer);

	if (status == kCMBlockBufferNoErr)
	{
		status = CMBlockBufferReplaceDataBytes(
			sampleData,
			blockBuffer,
			0,
			sampleSize);
	}

	if (status != kCMBlockBufferNoErr || !blockBuffer)
	{
		++videoToolboxErrorCount_;
		if (videoToolboxErrorCount_ <= 5 || (videoToolboxErrorCount_ % 60) == 0)
		{
			UE_LOG(LogTemp, Warning, TEXT("HevcDecoder(VideoToolbox): CMBlockBuffer setup failed status=%d (%s)"), (int)status, DecodeVtStatus(status));
		}
		return false;
	}

	CMSampleBufferRef sampleBuffer = nullptr;
	const size_t sampleSizes[] = { sampleSize };
	status = CMSampleBufferCreateReady(
		kCFAllocatorDefault,
		blockBuffer,
		videoToolboxFormatDescription_,
		1,
		0,
		nullptr,
		1,
		sampleSizes,
		&sampleBuffer);

	if (status != noErr || !sampleBuffer)
	{
		++videoToolboxErrorCount_;
		if (videoToolboxErrorCount_ <= 5 || (videoToolboxErrorCount_ % 60) == 0)
		{
			UE_LOG(LogTemp, Warning, TEXT("HevcDecoder(VideoToolbox): CMSampleBufferCreateReady failed status=%d (%s)"), (int)status, DecodeVtStatus(status));
		}
		CFRelease(blockBuffer);
		return false;
	}

	videoToolboxDecodedFrameReady_ = false;

	VTDecodeInfoFlags decodeInfoFlags = 0;
	status = VTDecompressionSessionDecodeFrame(
		videoToolboxSession_,
		sampleBuffer,
		kVTDecodeFrame_EnableAsynchronousDecompression,
		nullptr,
		&decodeInfoFlags);

	if (status == noErr)
	{
		const OSStatus waitStatus = VTDecompressionSessionWaitForAsynchronousFrames(videoToolboxSession_);
		if (waitStatus != noErr)
		{
			++videoToolboxErrorCount_;
			if (videoToolboxErrorCount_ <= 5 || (videoToolboxErrorCount_ % 60) == 0)
			{
				UE_LOG(LogTemp, Warning, TEXT("HevcDecoder(VideoToolbox): VTDecompressionSessionWaitForAsynchronousFrames failed status=%d (%s)"), (int)waitStatus, DecodeVtStatus(waitStatus));
			}
		}
	}
	else
	{
		++videoToolboxErrorCount_;
		if (videoToolboxErrorCount_ <= 5 || (videoToolboxErrorCount_ % 60) == 0)
		{
			UE_LOG(LogTemp, Warning, TEXT("HevcDecoder(VideoToolbox): VTDecompressionSessionDecodeFrame failed status=%d (%s)"), (int)status, DecodeVtStatus(status));
		}
	}

	CFRelease(sampleBuffer);
	CFRelease(blockBuffer);

	return status == noErr && videoToolboxDecodedFrameReady_;
}

void HevcDecoder::DecompressionOutputCallback(
	void* decompressionOutputRefCon,
	void* sourceFrameRefCon,
	OSStatus status,
	VTDecodeInfoFlags infoFlags,
	CVImageBufferRef imageBuffer,
	CMTime presentationTimeStamp,
	CMTime presentationDuration)
{
	(void)sourceFrameRefCon;
	(void)infoFlags;
	(void)presentationTimeStamp;
	(void)presentationDuration;

	HevcDecoder* decoder = static_cast<HevcDecoder*>(decompressionOutputRefCon);
	if (decoder)
	{
		decoder->HandleDecodedFrame(status, imageBuffer);
	}
}

void HevcDecoder::HandleDecodedFrame(OSStatus status, CVImageBufferRef imageBuffer)
{
	videoToolboxDecodedFrameReady_ = false;

	if (status != noErr || !imageBuffer)
	{
		++videoToolboxErrorCount_;
		if (videoToolboxErrorCount_ <= 5 || (videoToolboxErrorCount_ % 60) == 0)
		{
			UE_LOG(LogTemp, Warning, TEXT("HevcDecoder(VideoToolbox): decode callback status=%d (%s)"), (int)status, DecodeVtStatus(status));
		}
		return;
	}

	CVPixelBufferRef pixelBuffer = static_cast<CVPixelBufferRef>(imageBuffer);
	const size_t width = CVPixelBufferGetWidth(pixelBuffer);
	const size_t height = CVPixelBufferGetHeight(pixelBuffer);

	if (width == 0 || height == 0 || (width % 2) != 0 || (height % 2) != 0)
	{
		++videoToolboxErrorCount_;
		UE_LOG(LogTemp, Warning, TEXT("HevcDecoder(VideoToolbox): invalid decoded frame size %dx%d"), static_cast<int>(width), static_cast<int>(height));
		return;
	}

	outputSize_ = static_cast<uint32_t>(width * height * 3 / 2);
	if (bufferSizes_[bufferIndex_] != outputSize_)
	{
		delete[] buffers_[bufferIndex_];
		buffers_[bufferIndex_] = new uint8_t[outputSize_];
		bufferSizes_[bufferIndex_] = outputSize_;
	}

	uint8_t* dst = buffers_[bufferIndex_];
	uint8_t* dstY = dst;
	uint8_t* dstU = dst + width * height;
	uint8_t* dstV = dstU + (width * height / 4);

	if (CVPixelBufferLockBaseAddress(pixelBuffer, kCVPixelBufferLock_ReadOnly) != kCVReturnSuccess)
	{
		++videoToolboxErrorCount_;
		return;
	}

	const OSType pixelFormat = CVPixelBufferGetPixelFormatType(pixelBuffer);
	const size_t planeCount = CVPixelBufferGetPlaneCount(pixelBuffer);
	bool copied = false;

	if ((pixelFormat == kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange ||
		 pixelFormat == kCVPixelFormatType_420YpCbCr8BiPlanarFullRange) &&
		planeCount >= 2)
	{
		const uint8_t* yPlane = static_cast<const uint8_t*>(CVPixelBufferGetBaseAddressOfPlane(pixelBuffer, 0));
		const uint8_t* uvPlane = static_cast<const uint8_t*>(CVPixelBufferGetBaseAddressOfPlane(pixelBuffer, 1));
		const size_t yStride = CVPixelBufferGetBytesPerRowOfPlane(pixelBuffer, 0);
		const size_t uvStride = CVPixelBufferGetBytesPerRowOfPlane(pixelBuffer, 1);

		for (size_t row = 0; row < height; ++row)
		{
			std::memcpy(dstY + row * width, yPlane + row * yStride, width);
		}

		for (size_t row = 0; row < height / 2; ++row)
		{
			const uint8_t* uvRow = uvPlane + row * uvStride;
			for (size_t col = 0; col < width / 2; ++col)
			{
				dstU[row * (width / 2) + col] = uvRow[col * 2];
				dstV[row * (width / 2) + col] = uvRow[col * 2 + 1];
			}
		}

		copied = true;
	}
	else if (pixelFormat == kCVPixelFormatType_420YpCbCr8Planar && planeCount >= 3)
	{
		const uint8_t* yPlane = static_cast<const uint8_t*>(CVPixelBufferGetBaseAddressOfPlane(pixelBuffer, 0));
		const uint8_t* uPlane = static_cast<const uint8_t*>(CVPixelBufferGetBaseAddressOfPlane(pixelBuffer, 1));
		const uint8_t* vPlane = static_cast<const uint8_t*>(CVPixelBufferGetBaseAddressOfPlane(pixelBuffer, 2));

		const size_t yStride = CVPixelBufferGetBytesPerRowOfPlane(pixelBuffer, 0);
		const size_t uStride = CVPixelBufferGetBytesPerRowOfPlane(pixelBuffer, 1);
		const size_t vStride = CVPixelBufferGetBytesPerRowOfPlane(pixelBuffer, 2);

		for (size_t row = 0; row < height; ++row)
		{
			std::memcpy(dstY + row * width, yPlane + row * yStride, width);
		}

		for (size_t row = 0; row < height / 2; ++row)
		{
			std::memcpy(dstU + row * (width / 2), uPlane + row * uStride, width / 2);
			std::memcpy(dstV + row * (width / 2), vPlane + row * vStride, width / 2);
		}

		copied = true;
	}

	CVPixelBufferUnlockBaseAddress(pixelBuffer, kCVPixelBufferLock_ReadOnly);

	if (!copied)
	{
		++videoToolboxErrorCount_;
		if (videoToolboxErrorCount_ <= 5 || (videoToolboxErrorCount_ % 60) == 0)
		{
			UE_LOG(LogTemp, Warning, TEXT("HevcDecoder(VideoToolbox): unsupported pixel format 0x%x"), (unsigned)pixelFormat);
		}
		return;
	}

	outputData_ = buffers_[bufferIndex_];
	videoToolboxDecodedFrameReady_ = true;

	if (videoToolboxDecodedSampleCount_ < 10 || (videoToolboxDecodedSampleCount_ % 120) == 0)
	{
		UE_LOG(
			LogTemp,
			Log,
			TEXT("HevcDecoder(VideoToolbox): decoded frame ready %ux%u (%u bytes)"),
			static_cast<uint32_t>(width),
			static_cast<uint32_t>(height),
			outputSize_);
	}
}
#endif // CITHRUS_VIDEOTOOLBOX_AVAILABLE

void HevcDecoder::Process()
{
	GetOutputPin<0>().SetData(nullptr);
	GetOutputPin<0>().SetSize(0);
	outputData_ = nullptr;
	outputSize_ = 0;

	const uint8_t* inputData = GetInputPin<0>().GetData();
	const uint32_t inputSize = GetInputPin<0>().GetSize();

	if (!inputData || inputSize == 0 || !backendConfigured_)
	{
		return;
	}

	const bool hasAnnexBPrefix = StartsWithAnnexB(inputData, inputSize);

	std::vector<FNalUnitView> nalUnits;
	ExtractNalUnits(inputData, inputSize, hasAnnexBPrefix, nalUnits);
	if (nalUnits.empty())
	{
		return;
	}

	bool decodedFrame = false;

#if defined(CITHRUS_VIDEOTOOLBOX_AVAILABLE) && PLATFORM_MAC
	if (useVideoToolbox_)
	{
		auto appendNalToPendingAu = [this](const uint8_t* nalData, const uint32_t nalSize, const bool hasVcl)
		{
			videoToolboxPendingAuHvcc_.push_back(static_cast<uint8_t>((nalSize >> 24) & 0xFF));
			videoToolboxPendingAuHvcc_.push_back(static_cast<uint8_t>((nalSize >> 16) & 0xFF));
			videoToolboxPendingAuHvcc_.push_back(static_cast<uint8_t>((nalSize >> 8) & 0xFF));
			videoToolboxPendingAuHvcc_.push_back(static_cast<uint8_t>(nalSize & 0xFF));
			videoToolboxPendingAuHvcc_.insert(videoToolboxPendingAuHvcc_.end(), nalData, nalData + nalSize);
			videoToolboxPendingAuNalCount_++;
			videoToolboxPendingAuHasVcl_ = videoToolboxPendingAuHasVcl_ || hasVcl;
		};

		auto flushPendingAu = [this, &decodedFrame]() -> bool
		{
			if (!videoToolboxPendingAuHasVcl_ || videoToolboxPendingAuHvcc_.empty())
			{
				return true;
			}

			if (!EnsureVideoToolboxSession())
			{
				TryFallbackToOpenHevc("VideoToolbox session creation failed");
				return false;
			}

			const uint32_t submittedNalCount = videoToolboxPendingAuNalCount_;
			const uint32_t submittedBytes = static_cast<uint32_t>(videoToolboxPendingAuHvcc_.size());

			const bool gotFrame = DecodeWithVideoToolboxSample(
				videoToolboxPendingAuHvcc_.data(),
				static_cast<uint32_t>(videoToolboxPendingAuHvcc_.size()));

			videoToolboxSubmittedSampleCount_++;

			if (gotFrame)
			{
				videoToolboxDecodedSampleCount_++;
				videoToolboxNoOutputSampleCount_ = 0;
				decodedFrame = true;
			}
			else
			{
				videoToolboxNoOutputSampleCount_++;
				if (videoToolboxNoOutputSampleCount_ <= 5 || (videoToolboxNoOutputSampleCount_ % 60) == 0)
				{
					UE_LOG(
						LogTemp,
						Warning,
						TEXT("HevcDecoder(VideoToolbox): submitted AU but no frame output (noOutput=%u, submitted=%u, decoded=%u, auNals=%u, auBytes=%u)"),
						videoToolboxNoOutputSampleCount_,
						videoToolboxSubmittedSampleCount_,
						videoToolboxDecodedSampleCount_,
						submittedNalCount,
						submittedBytes);
				}
			}

			videoToolboxPendingAuHvcc_.clear();
			videoToolboxPendingAuHasVcl_ = false;
			videoToolboxPendingAuNalCount_ = 0;
			return true;
		};

		for (const FNalUnitView& nal : nalUnits)
		{
			if (!nal.data || nal.size == 0)
			{
				continue;
			}

			if (IsParameterSetNal(nal.type))
			{
				CacheParameterSet(nal.data, nal.size, nal.type);
				appendNalToPendingAu(nal.data, nal.size, false);
				continue;
			}

			const bool isVcl = IsVclNal(nal.type);
			if (!isVcl)
			{
				appendNalToPendingAu(nal.data, nal.size, false);
				continue;
			}

			const bool firstSlice = IsFirstSliceSegmentInPicture(nal.data, nal.size, nal.type);
			if (videoToolboxNalLogCount_ < 120)
			{
				UE_LOG(LogTemp, Log, TEXT("HevcDecoder(VideoToolbox): VCL NAL type=%u size=%u firstSlice=%d pendingHasVcl=%d pendingNals=%u"),
					static_cast<uint32_t>(nal.type),
					nal.size,
					firstSlice ? 1 : 0,
					videoToolboxPendingAuHasVcl_ ? 1 : 0,
					videoToolboxPendingAuNalCount_);
				videoToolboxNalLogCount_++;
			}

			if (firstSlice && videoToolboxPendingAuHasVcl_)
			{
				if (!flushPendingAu())
				{
					break;
				}
			}

			appendNalToPendingAu(nal.data, nal.size, true);

			if (videoToolboxPendingAuHvcc_.size() > 2 * 1024 * 1024)
			{
				UE_LOG(LogTemp, Warning, TEXT("HevcDecoder(VideoToolbox): pending AU grew too large (%u bytes), flushing heuristically"),
					static_cast<uint32_t>(videoToolboxPendingAuHvcc_.size()));
				if (!flushPendingAu())
				{
					break;
				}
			}
		}

		// If we got a complete single-NAL VCL frame in this packet and no boundary appeared,
		// allow immediate decode to avoid waiting forever on boundary-less streams.
		if (!decodedFrame && nalUnits.size() == 1 && IsVclNal(nalUnits[0].type) && videoToolboxPendingAuHasVcl_)
		{
			const bool firstSlice = IsFirstSliceSegmentInPicture(nalUnits[0].data, nalUnits[0].size, nalUnits[0].type);
			if (firstSlice)
			{
				flushPendingAu();
			}
		}
	}
#endif // CITHRUS_VIDEOTOOLBOX_AVAILABLE && PLATFORM_MAC

	if (!decodedFrame && useOpenHevc_)
	{
		decodedFrame = DecodeWithOpenHevc(inputData, inputSize, hasAnnexBPrefix);
	}

	if (!decodedFrame || !outputData_ || outputSize_ == 0)
	{
		return;
	}

	GetOutputPin<0>().SetData(outputData_);
	GetOutputPin<0>().SetSize(outputSize_);
	bufferIndex_ = (bufferIndex_ + 1) % 2;
}
