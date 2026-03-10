#include "HevcEncoder.h"

#include "Misc/Debug.h"
#include "StreamPerfStats.h"

#include <algorithm>
#include <cstring>
#include <memory>
#include <thread>

#ifdef CITHRUS_VIDEOTOOLBOX_AVAILABLE
#include <CoreFoundation/CoreFoundation.h>
#include <CoreMedia/CoreMedia.h>
#include <CoreVideo/CoreVideo.h>
#include <VideoToolbox/VideoToolbox.h>
static const TCHAR* DecodeCVStatus(OSStatus status);
#endif

const uint64_t KVAZAAR_FRAMERATE_DENOM = 90000;

namespace
{
	inline uint8_t ClampToByte(int value)
	{
		return static_cast<uint8_t>(std::min(std::max(value, 0), 255));
	}

	inline uint8_t RgbToY(uint8_t r, uint8_t g, uint8_t b)
	{
		return static_cast<uint8_t>((76 * r + 150 * g + 29 * b) >> 8);
	}

	inline uint8_t RgbToU(uint8_t r, uint8_t g, uint8_t b)
	{
		return ClampToByte(((-43 * r - 84 * g + 127 * b) >> 8) + 128);
	}

	inline uint8_t RgbToV(uint8_t r, uint8_t g, uint8_t b)
	{
		return ClampToByte(((127 * r - 106 * g - 21 * b) >> 8) + 128);
	}

	inline void LoadRgb(const uint8_t* pixel, bool isRGBA, uint8_t& r, uint8_t& g, uint8_t& b)
	{
		r = isRGBA ? pixel[0] : pixel[2];
		g = pixel[1];
		b = isRGBA ? pixel[2] : pixel[0];
	}

	void ConvertRgbInputToI420(const uint8_t* inputData, uint32_t width, uint32_t height, bool isRGBA, std::vector<uint8_t>& outputData)
	{
		outputData.resize(width * height * 3 / 2);
		uint8_t* yPlane = outputData.data();
		uint8_t* uPlane = yPlane + width * height;
		uint8_t* vPlane = uPlane + (width * height / 4);

		for (uint32_t y = 0; y < height; ++y)
		{
			const uint8_t* srcRow = inputData + (y * width * 4);
			uint8_t* yRow = yPlane + (y * width);
			for (uint32_t x = 0; x < width; ++x)
			{
				uint8_t r, g, b;
				LoadRgb(srcRow + (x * 4), isRGBA, r, g, b);
				yRow[x] = RgbToY(r, g, b);
			}
		}

		for (uint32_t y = 0; y < height; y += 2)
		{
			const uint8_t* row0 = inputData + (y * width * 4);
			const uint8_t* row1 = inputData + (std::min(y + 1, height - 1) * width * 4);
			uint8_t* uRow = uPlane + ((y / 2) * (width / 2));
			uint8_t* vRow = vPlane + ((y / 2) * (width / 2));

			for (uint32_t x = 0; x < width; x += 2)
			{
				const uint32_t x1 = std::min(x + 1, width - 1);
				uint8_t r00, g00, b00;
				uint8_t r01, g01, b01;
				uint8_t r10, g10, b10;
				uint8_t r11, g11, b11;
				LoadRgb(row0 + (x * 4), isRGBA, r00, g00, b00);
				LoadRgb(row0 + (x1 * 4), isRGBA, r01, g01, b01);
				LoadRgb(row1 + (x * 4), isRGBA, r10, g10, b10);
				LoadRgb(row1 + (x1 * 4), isRGBA, r11, g11, b11);
				const uint8_t r = static_cast<uint8_t>((r00 + r01 + r10 + r11 + 2) >> 2);
				const uint8_t g = static_cast<uint8_t>((g00 + g01 + g10 + g11 + 2) >> 2);
				const uint8_t b = static_cast<uint8_t>((b00 + b01 + b10 + b11 + 2) >> 2);
				uRow[x / 2] = RgbToU(r, g, b);
				vRow[x / 2] = RgbToV(r, g, b);
			}
		}
	}

	FString BackendToString(EHevcEncoderBackend backend)
	{
		switch (backend)
		{
		case EHevcEncoderBackend::Auto:
			return TEXT("Auto");
		case EHevcEncoderBackend::VideoToolbox:
			return TEXT("VideoToolbox");
		case EHevcEncoderBackend::Kvazaar:
			return TEXT("Kvazaar");
		default:
			return TEXT("Unknown");
		}
	}

#ifdef CITHRUS_VIDEOTOOLBOX_AVAILABLE
	FString PixelFormatToString(OSType pixelFormat)
	{
		switch (pixelFormat)
		{
		case kCVPixelFormatType_420YpCbCr8BiPlanarFullRange:
			return TEXT("NV12 FullRange");
		case kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange:
			return TEXT("NV12 VideoRange");
		case kCVPixelFormatType_420YpCbCr8Planar:
			return TEXT("I420 Planar");
#ifdef kCVPixelFormatType_420YpCbCr8PlanarFullRange
		case kCVPixelFormatType_420YpCbCr8PlanarFullRange:
			return TEXT("I420 FullRange");
#endif
		case kCVPixelFormatType_32BGRA:
			return TEXT("BGRA");
		default:
			return FString::Printf(TEXT("0x%x"), static_cast<unsigned>(pixelFormat));
		}
	}
#endif
}

EHevcEncoderBackend HevcEncoder::ResolveBackend(const EHevcEncoderBackend& requestedBackend)
{
#if defined(CITHRUS_VIDEOTOOLBOX_AVAILABLE) && PLATFORM_MAC
	const bool canUseVideoToolbox = true;
#else
	const bool canUseVideoToolbox = false;
#endif
#ifdef CITHRUS_KVAZAAR_AVAILABLE
	const bool canUseKvazaar = true;
#else
	const bool canUseKvazaar = false;
#endif

	switch (requestedBackend)
	{
	case EHevcEncoderBackend::VideoToolbox:
		if (canUseVideoToolbox)
		{
			return EHevcEncoderBackend::VideoToolbox;
		}
		return canUseKvazaar ? EHevcEncoderBackend::Kvazaar : EHevcEncoderBackend::Auto;

	case EHevcEncoderBackend::Kvazaar:
		if (canUseKvazaar)
		{
			return EHevcEncoderBackend::Kvazaar;
		}
		return canUseVideoToolbox ? EHevcEncoderBackend::VideoToolbox : EHevcEncoderBackend::Auto;

	case EHevcEncoderBackend::Auto:
	default:
#if defined(CITHRUS_VIDEOTOOLBOX_AVAILABLE) && PLATFORM_MAC
		if (canUseVideoToolbox && IsHardwareVideoToolboxAvailable())
		{
			return EHevcEncoderBackend::VideoToolbox;
		}
#endif
		if (canUseKvazaar)
		{
			return EHevcEncoderBackend::Kvazaar;
		}
		return canUseVideoToolbox ? EHevcEncoderBackend::VideoToolbox : EHevcEncoderBackend::Auto;
	}
}

HevcEncoder::HevcEncoder(
	const uint16_t& frameWidth,
	const uint16_t& frameHeight,
	const uint8_t& threadCount,
	const uint8_t& qp,
	const uint8_t& wpp,
	const uint8_t& owf,
	const HevcEncoderPreset& preset,
	const EHevcEncoderBackend& requestedBackend,
	const float& targetBitrateMbps,
	const uint32_t& maxKeyFrameInterval,
	const uint32_t& expectedFrameRate,
	const uint32_t& perfLogInterval,
	const uint32_t& maxPendingFrames)
	: frameWidth_(frameWidth)
	, frameHeight_(frameHeight)
	, outputData_(nullptr)
	, startTime_(std::chrono::high_resolution_clock::time_point::min())
	, requestedBackend_(requestedBackend)
	, selectedBackend_(EHevcEncoderBackend::Auto)
	, preset_(preset)
	, threadCount_(threadCount)
	, qp_(qp)
	, wpp_(wpp)
	, owf_(owf)
	, targetBitrateMbps_(targetBitrateMbps)
	, maxKeyFrameInterval_(std::max<uint32_t>(1, maxKeyFrameInterval))
	, expectedFrameRate_(std::max<uint32_t>(1, expectedFrameRate))
	, perfLogInterval_(std::max<uint32_t>(1, perfLogInterval))
	, maxPendingFrames_(std::max<uint32_t>(2, maxPendingFrames))
	, perfFramesAccumulated_(0)
	, droppedFrames_(0)
{
	GetInputPin<0>().SetAcceptedFormats({ "yuv420", "rgba", "bgra" });
	GetOutputPin<0>().SetFormat("hevc");

	const EHevcEncoderBackend preferredBackend = ResolveBackend(requestedBackend_);

#if defined(CITHRUS_VIDEOTOOLBOX_AVAILABLE) && PLATFORM_MAC
	if (preferredBackend == EHevcEncoderBackend::VideoToolbox)
	{
		const bool requireHardware = (requestedBackend_ == EHevcEncoderBackend::Auto);
		if (InitializeVideoToolbox(requireHardware))
		{
			return;
		}
	}
#endif

#ifdef CITHRUS_KVAZAAR_AVAILABLE
	if (preferredBackend == EHevcEncoderBackend::Kvazaar || requestedBackend_ == EHevcEncoderBackend::Auto)
	{
		if (InitializeKvazaar())
		{
			return;
		}
	}
#endif

#if defined(CITHRUS_VIDEOTOOLBOX_AVAILABLE) && PLATFORM_MAC
	if (!useVideoToolbox_ && InitializeVideoToolbox(false))
	{
		return;
	}
#endif

#ifdef CITHRUS_KVAZAAR_AVAILABLE
	if (!kvazaarEncoder_ && InitializeKvazaar())
	{
		return;
	}
#endif

	UE_LOG(LogTemp, Error, TEXT("HevcEncoder: No HEVC encoder backend could be initialized"));
}

HevcEncoder::~HevcEncoder()
{
#ifdef CITHRUS_VIDEOTOOLBOX_AVAILABLE
	ShutdownVideoToolbox();
#endif
#ifdef CITHRUS_KVAZAAR_AVAILABLE
	ShutdownKvazaar();
#endif
	ClearOutputFrame();
}

void HevcEncoder::ClearOutputFrame()
{
	delete[] outputData_;
	outputData_ = nullptr;
	GetOutputPin<0>().SetData(nullptr);
	GetOutputPin<0>().SetSize(0);
}

void HevcEncoder::PublishOutputFrame(const std::vector<uint8_t>& frameData)
{
	ClearOutputFrame();
	if (frameData.empty())
	{
		return;
	}

	outputData_ = new uint8_t[frameData.size()];
	memcpy(outputData_, frameData.data(), frameData.size());
	GetOutputPin<0>().SetData(outputData_);
	GetOutputPin<0>().SetSize(static_cast<uint32_t>(frameData.size()));
}

FString HevcEncoder::BackendName() const
{
	return BackendToString(selectedBackend_);
}

void HevcEncoder::Process()
{
	const uint8_t* inputData = GetInputPin<0>().GetData();
	const uint32_t inputSize = GetInputPin<0>().GetSize();
	const std::string inputFormat = GetInputPin<0>().GetFormat();

	if (inputData && inputSize > 0 && startTime_ == std::chrono::high_resolution_clock::time_point::min())
	{
		startTime_ = std::chrono::high_resolution_clock::now();
	}

#if defined(CITHRUS_VIDEOTOOLBOX_AVAILABLE) && PLATFORM_MAC
	if (selectedBackend_ == EHevcEncoderBackend::VideoToolbox)
	{
		DrainCompletedVideoToolboxFrames();
		if (inputData && inputSize > 0)
		{
			EncodeWithVideoToolbox(inputData, inputSize, inputFormat);
		}
		LogPerformanceIfNeeded();
		return;
	}
#endif

#ifdef CITHRUS_KVAZAAR_AVAILABLE
	if (selectedBackend_ == EHevcEncoderBackend::Kvazaar)
	{
		if (!inputData || inputSize == 0 || !EncodeWithKvazaar(inputData, inputSize, inputFormat))
		{
			ClearOutputFrame();
		}
		return;
	}
#endif

	ClearOutputFrame();
}

void HevcEncoder::LogPerformanceIfNeeded()
{
#if defined(CITHRUS_VIDEOTOOLBOX_AVAILABLE) && PLATFORM_MAC
	if (selectedBackend_ != EHevcEncoderBackend::VideoToolbox)
	{
		return;
	}

	uint32_t perfFrames = 0;
	double avgAllocMs = 0.0;
	double avgConvertMs = 0.0;
	double avgEncodeMs = 0.0;
	uint32_t pendingFrames = 0;
	uint64_t droppedFrames = 0;
	{
		std::lock_guard<std::mutex> lock(encodeMutex_);
		if (perfFramesAccumulated_ < perfLogInterval_)
		{
			return;
		}

		perfFrames = perfFramesAccumulated_;
		avgAllocMs = vtAllocSamples_ > 0 ? (vtAllocMsTotal_ / static_cast<double>(vtAllocSamples_)) : 0.0;
		avgConvertMs = vtConvertSamples_ > 0 ? (vtConvertMsTotal_ / static_cast<double>(vtConvertSamples_)) : 0.0;
		avgEncodeMs = vtEncodeSamples_ > 0 ? (vtEncodeMsTotal_ / static_cast<double>(vtEncodeSamples_)) : 0.0;
		pendingFrames = pendingEncodeCount_;
		droppedFrames = droppedFrames_;

		perfFramesAccumulated_ = 0;
		vtAllocSamples_ = 0;
		vtConvertSamples_ = 0;
		vtEncodeSamples_ = 0;
		vtAllocMsTotal_ = 0.0;
		vtConvertMsTotal_ = 0.0;
		vtEncodeMsTotal_ = 0.0;
		droppedFrames_ = 0;
		droppedCompletedFrames_ = 0;
	}

	const FStreamPerfSnapshot snapshot = StreamPerfStats::Consume();
	const uint64_t totalDroppedFrames = droppedFrames + snapshot.ReaderDroppedFrames + snapshot.RtpDroppedFrames;
	UE_LOG(
		LogTemp,
		Log,
		TEXT("HevcEncoder PERF [%s hw=%d frames=%u]: readback=%.2fms convert=%.2fms pixelBuffer=%.2fms encode=%.2fms rtp=%.2fms pending=%u dropped=%llu"),
		*BackendName(),
		usingHardwareVideoToolbox_ ? 1 : 0,
		perfFrames,
		snapshot.ReadbackMs,
		avgConvertMs,
		avgAllocMs,
		avgEncodeMs,
		snapshot.RtpSendMs,
		pendingFrames,
		static_cast<unsigned long long>(totalDroppedFrames));
#endif
}

#ifdef CITHRUS_KVAZAAR_AVAILABLE
bool HevcEncoder::InitializeKvazaar()
{
	if (!kvazaarApi_)
	{
		return false;
	}

	kvazaarConfig_ = kvazaarApi_->config_alloc();
	if (!kvazaarConfig_)
	{
		return false;
	}

	kvazaarApi_->config_init(kvazaarConfig_);
	kvazaarApi_->config_parse(kvazaarConfig_, "threads", std::to_string(threadCount_).c_str());

	switch (preset_)
	{
	case HevcPresetMinimumLatency:
		kvazaarApi_->config_parse(kvazaarConfig_, "preset", "ultrafast");
		kvazaarApi_->config_parse(kvazaarConfig_, "gop", "lp-g8d1t1");
		kvazaarApi_->config_parse(kvazaarConfig_, "vps-period", "16");
		kvazaarConfig_->qp = qp_;
		kvazaarConfig_->wpp = wpp_;
		kvazaarConfig_->owf = owf_;
		break;

	case HevcPresetLossless:
		kvazaarConfig_->lossless = 1;
		break;

	default:
		kvazaarConfig_->qp = qp_;
		kvazaarConfig_->wpp = wpp_;
		kvazaarConfig_->owf = owf_;
		break;
	}

	kvazaarConfig_->width = frameWidth_;
	kvazaarConfig_->height = frameHeight_;
	kvazaarConfig_->hash = KVZ_HASH_NONE;
	kvazaarConfig_->aud_enable = 0;
	kvazaarConfig_->calc_psnr = 0;

	kvazaarEncoder_ = kvazaarApi_->encoder_open(kvazaarConfig_);
	if (!kvazaarEncoder_)
	{
		ShutdownKvazaar();
		return false;
	}

	kvazaarTransmitPicture_ = kvazaarApi_->picture_alloc(frameWidth_, frameHeight_);
	if (!kvazaarTransmitPicture_)
	{
		ShutdownKvazaar();
		return false;
	}

	selectedBackend_ = EHevcEncoderBackend::Kvazaar;
	UE_LOG(LogTemp, Log, TEXT("HevcEncoder: Using Kvazaar backend"));
	return true;
}

void HevcEncoder::ShutdownKvazaar()
{
	if (kvazaarTransmitPicture_)
	{
		kvazaarApi_->picture_free(kvazaarTransmitPicture_);
		kvazaarTransmitPicture_ = nullptr;
	}

	if (kvazaarEncoder_)
	{
		kvazaarApi_->encoder_close(kvazaarEncoder_);
		kvazaarEncoder_ = nullptr;
	}

	if (kvazaarConfig_)
	{
		kvazaarApi_->config_destroy(kvazaarConfig_);
		kvazaarConfig_ = nullptr;
	}
}

bool HevcEncoder::EncodeWithKvazaar(const uint8_t* inputData, uint32_t inputSize, const std::string& inputFormat)
{
	if (!kvazaarEncoder_ || !kvazaarTransmitPicture_)
	{
		return false;
	}

	const uint8_t* yuvFrame = inputData;
	if (inputFormat == "bgra" || inputFormat == "rgba")
	{
		ConvertRgbInputToI420(inputData, frameWidth_, frameHeight_, inputFormat == "rgba", kvazaarScratchBuffer_);
		yuvFrame = kvazaarScratchBuffer_.data();
		inputSize = static_cast<uint32_t>(kvazaarScratchBuffer_.size());
	}

	const uint32_t expectedSize = frameWidth_ * frameHeight_ * 3 / 2;
	if (!yuvFrame || inputSize < expectedSize)
	{
		return false;
	}

	memcpy(kvazaarTransmitPicture_->y, yuvFrame, frameWidth_ * frameHeight_);
	yuvFrame += frameWidth_ * frameHeight_;
	memcpy(kvazaarTransmitPicture_->u, yuvFrame, frameWidth_ * frameHeight_ / 4);
	yuvFrame += frameWidth_ * frameHeight_ / 4;
	memcpy(kvazaarTransmitPicture_->v, yuvFrame, frameWidth_ * frameHeight_ / 4);

	kvz_frame_info frameInfo;
	kvz_data_chunk* dataOut = nullptr;
	uint32_t lenOut = 0;
	kvazaarApi_->encoder_encode(kvazaarEncoder_, kvazaarTransmitPicture_, &dataOut, &lenOut, nullptr, nullptr, &frameInfo);

	if (!dataOut)
	{
		return false;
	}

	std::vector<uint8_t> encodedFrame(lenOut);
	uint8_t* dataPtr = encodedFrame.data();
	for (kvz_data_chunk* chunk = dataOut; chunk != nullptr; chunk = chunk->next)
	{
		memcpy(dataPtr, chunk->data, chunk->len);
		dataPtr += chunk->len;
	}

	kvazaarApi_->chunk_free(dataOut);
	kvazaarApi_->picture_free(kvazaarTransmitPicture_);
	kvazaarTransmitPicture_ = kvazaarApi_->picture_alloc(frameWidth_, frameHeight_);

	PublishOutputFrame(encodedFrame);
	return kvazaarTransmitPicture_ != nullptr;
}
#endif // CITHRUS_KVAZAAR_AVAILABLE

#ifdef CITHRUS_VIDEOTOOLBOX_AVAILABLE
bool HevcEncoder::SupportsVideoToolbox()
{
#if PLATFORM_MAC
	return true;
#else
	return false;
#endif
}

bool HevcEncoder::IsHardwareVideoToolboxAvailable()
{
#if !PLATFORM_MAC
	return false;
#else
	CFMutableDictionaryRef encoderSpec = CFDictionaryCreateMutable(
		kCFAllocatorDefault,
		2,
		&kCFTypeDictionaryKeyCallBacks,
		&kCFTypeDictionaryValueCallBacks);
	CFDictionarySetValue(encoderSpec, kVTVideoEncoderSpecification_EnableHardwareAcceleratedVideoEncoder, kCFBooleanTrue);
	CFDictionarySetValue(encoderSpec, kVTVideoEncoderSpecification_RequireHardwareAcceleratedVideoEncoder, kCFBooleanTrue);

	CFMutableDictionaryRef pixelBufferAttrs = CFDictionaryCreateMutable(
		kCFAllocatorDefault,
		3,
		&kCFTypeDictionaryKeyCallBacks,
		&kCFTypeDictionaryValueCallBacks);
	int32_t width = 64;
	int32_t height = 64;
	CFNumberRef widthNum = CFNumberCreate(kCFAllocatorDefault, kCFNumberSInt32Type, &width);
	CFNumberRef heightNum = CFNumberCreate(kCFAllocatorDefault, kCFNumberSInt32Type, &height);
	CFMutableDictionaryRef ioSurfProps = CFDictionaryCreateMutable(kCFAllocatorDefault, 0, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
	CFDictionarySetValue(pixelBufferAttrs, kCVPixelBufferWidthKey, widthNum);
	CFDictionarySetValue(pixelBufferAttrs, kCVPixelBufferHeightKey, heightNum);
	CFDictionarySetValue(pixelBufferAttrs, kCVPixelBufferIOSurfacePropertiesKey, ioSurfProps);
	CFRelease(widthNum);
	CFRelease(heightNum);
	CFRelease(ioSurfProps);

	VTCompressionSessionRef session = nullptr;
	const OSStatus createStatus = VTCompressionSessionCreate(
		kCFAllocatorDefault,
		width,
		height,
		kCMVideoCodecType_HEVC,
		encoderSpec,
		pixelBufferAttrs,
		kCFAllocatorDefault,
		nullptr,
		nullptr,
		&session);

	CFRelease(pixelBufferAttrs);
	CFRelease(encoderSpec);

	if (createStatus != noErr || !session)
	{
		return false;
	}

	bool usingHardware = false;
	CFTypeRef usingHardwareValue = nullptr;
	if (VTSessionCopyProperty(session, kVTCompressionPropertyKey_UsingHardwareAcceleratedVideoEncoder, kCFAllocatorDefault, &usingHardwareValue) == noErr && usingHardwareValue)
	{
		if (CFGetTypeID(usingHardwareValue) == CFBooleanGetTypeID())
		{
			usingHardware = CFBooleanGetValue(static_cast<CFBooleanRef>(usingHardwareValue));
		}
		CFRelease(usingHardwareValue);
	}

	VTCompressionSessionInvalidate(session);
	CFRelease(session);
	return usingHardware;
#endif
}

bool HevcEncoder::InitializeVideoToolbox(bool requireHardware)
{
#if !PLATFORM_MAC
	return false;
#else
	if (!SupportsVideoToolbox())
	{
		return false;
	}

	CFMutableDictionaryRef encoderSpec = CFDictionaryCreateMutable(
		kCFAllocatorDefault,
		2,
		&kCFTypeDictionaryKeyCallBacks,
		&kCFTypeDictionaryValueCallBacks);
	CFDictionarySetValue(encoderSpec, kVTVideoEncoderSpecification_EnableHardwareAcceleratedVideoEncoder, kCFBooleanTrue);
	if (requireHardware)
	{
		CFDictionarySetValue(encoderSpec, kVTVideoEncoderSpecification_RequireHardwareAcceleratedVideoEncoder, kCFBooleanTrue);
	}

	CFMutableDictionaryRef pixelBufferAttrs = CFDictionaryCreateMutable(
		kCFAllocatorDefault,
		3,
		&kCFTypeDictionaryKeyCallBacks,
		&kCFTypeDictionaryValueCallBacks);
	CFMutableDictionaryRef ioSurfProps = CFDictionaryCreateMutable(kCFAllocatorDefault, 0, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
	CFNumberRef widthNum = CFNumberCreate(kCFAllocatorDefault, kCFNumberSInt32Type, &frameWidth_);
	CFNumberRef heightNum = CFNumberCreate(kCFAllocatorDefault, kCFNumberSInt32Type, &frameHeight_);
	CFDictionarySetValue(pixelBufferAttrs, kCVPixelBufferWidthKey, widthNum);
	CFDictionarySetValue(pixelBufferAttrs, kCVPixelBufferHeightKey, heightNum);
	CFDictionarySetValue(pixelBufferAttrs, kCVPixelBufferIOSurfacePropertiesKey, ioSurfProps);
	CFRelease(widthNum);
	CFRelease(heightNum);
	CFRelease(ioSurfProps);

	if (!callbackState_)
	{
		callbackState_ = new VideoToolboxCallbackState();
	}

	{
		std::lock_guard<std::mutex> callbackLock(callbackState_->Mutex);
		callbackState_->Encoder = this;
		callbackState_->ActiveCallbacks = 0;
	}

	const OSStatus status = VTCompressionSessionCreate(
		kCFAllocatorDefault,
		frameWidth_,
		frameHeight_,
		kCMVideoCodecType_HEVC,
		encoderSpec,
		pixelBufferAttrs,
		kCFAllocatorDefault,
		CompressionCallback,
		callbackState_,
		&compressionSession_);

	CFRelease(pixelBufferAttrs);
	CFRelease(encoderSpec);

	if (status != noErr || !compressionSession_)
	{
		if (callbackState_)
		{
			delete callbackState_;
			callbackState_ = nullptr;
		}
		UE_LOG(LogTemp, Warning, TEXT("HevcEncoder: VTCompressionSessionCreate failed status=%d"), static_cast<int>(status));
		return false;
	}

	OSStatus propertyStatus = VTSessionSetProperty(compressionSession_, kVTCompressionPropertyKey_RealTime, kCFBooleanTrue);
	if (propertyStatus != noErr) { UE_LOG(LogTemp, Warning, TEXT("VT: Failed to set RealTime: %d"), static_cast<int>(propertyStatus)); }
	propertyStatus = VTSessionSetProperty(compressionSession_, kVTCompressionPropertyKey_AllowFrameReordering, kCFBooleanFalse);
	if (propertyStatus != noErr) { UE_LOG(LogTemp, Warning, TEXT("VT: Failed to set AllowFrameReordering: %d"), static_cast<int>(propertyStatus)); }
	int32_t maxDelay = 1;
	CFNumberRef maxDelayNum = CFNumberCreate(kCFAllocatorDefault, kCFNumberSInt32Type, &maxDelay);
	propertyStatus = VTSessionSetProperty(compressionSession_, kVTCompressionPropertyKey_MaxFrameDelayCount, maxDelayNum);
	CFRelease(maxDelayNum);
	if (propertyStatus != noErr && propertyStatus != kVTPropertyNotSupportedErr)
	{
		UE_LOG(LogTemp, Warning, TEXT("VT: Failed to set MaxFrameDelayCount: %d"), static_cast<int>(propertyStatus));
	}

	int32_t gop = static_cast<int32_t>(maxKeyFrameInterval_);
	CFNumberRef gopNum = CFNumberCreate(kCFAllocatorDefault, kCFNumberSInt32Type, &gop);
	propertyStatus = VTSessionSetProperty(compressionSession_, kVTCompressionPropertyKey_MaxKeyFrameInterval, gopNum);
	CFRelease(gopNum);
	if (propertyStatus != noErr) { UE_LOG(LogTemp, Warning, TEXT("VT: Failed to set MaxKeyFrameInterval: %d"), static_cast<int>(propertyStatus)); }

	double gopDurationValue = static_cast<double>(maxKeyFrameInterval_) / static_cast<double>(expectedFrameRate_);
	CFNumberRef gopDuration = CFNumberCreate(kCFAllocatorDefault, kCFNumberDoubleType, &gopDurationValue);
	propertyStatus = VTSessionSetProperty(compressionSession_, kVTCompressionPropertyKey_MaxKeyFrameIntervalDuration, gopDuration);
	CFRelease(gopDuration);
	if (propertyStatus != noErr) { UE_LOG(LogTemp, Warning, TEXT("VT: Failed to set MaxKeyFrameIntervalDuration: %d"), static_cast<int>(propertyStatus)); }

	if (preset_ == HevcPresetLossless)
	{
		float qualityValue = 1.0f;
		CFNumberRef qualityNum = CFNumberCreate(kCFAllocatorDefault, kCFNumberFloatType, &qualityValue);
		propertyStatus = VTSessionSetProperty(compressionSession_, kVTCompressionPropertyKey_Quality, qualityNum);
		CFRelease(qualityNum);
		if (propertyStatus != noErr) { UE_LOG(LogTemp, Warning, TEXT("VT: Failed to set Quality: %d"), static_cast<int>(propertyStatus)); }
	}
	else
	{
		int32_t targetBitrate = static_cast<int32_t>(std::max(0.1f, targetBitrateMbps_) * 1024.0f * 1024.0f);
		CFNumberRef bitrateNum = CFNumberCreate(kCFAllocatorDefault, kCFNumberSInt32Type, &targetBitrate);
		propertyStatus = VTSessionSetProperty(compressionSession_, kVTCompressionPropertyKey_AverageBitRate, bitrateNum);
		CFRelease(bitrateNum);
		if (propertyStatus != noErr) { UE_LOG(LogTemp, Warning, TEXT("VT: Failed to set AverageBitRate: %d"), static_cast<int>(propertyStatus)); }

		int32_t dataRateLimits[2] = { targetBitrate / 8, 1 };
		CFNumberRef dataRateNum = CFNumberCreate(kCFAllocatorDefault, kCFNumberSInt32Type, &dataRateLimits[0]);
		CFNumberRef dataRateDuration = CFNumberCreate(kCFAllocatorDefault, kCFNumberSInt32Type, &dataRateLimits[1]);
		const void* limitValues[] = { dataRateNum, dataRateDuration };
		CFArrayRef dataRateLimitsArray = CFArrayCreate(kCFAllocatorDefault, limitValues, 2, &kCFTypeArrayCallBacks);
		propertyStatus = VTSessionSetProperty(compressionSession_, kVTCompressionPropertyKey_DataRateLimits, dataRateLimitsArray);
		CFRelease(dataRateNum);
		CFRelease(dataRateDuration);
		CFRelease(dataRateLimitsArray);
		if (propertyStatus != noErr) { UE_LOG(LogTemp, Warning, TEXT("VT: Failed to set DataRateLimits: %d"), static_cast<int>(propertyStatus)); }
	}

	propertyStatus = VTSessionSetProperty(compressionSession_, kVTCompressionPropertyKey_ProfileLevel, kVTProfileLevel_HEVC_Main_AutoLevel);
	if (propertyStatus != noErr) { UE_LOG(LogTemp, Warning, TEXT("VT: Failed to set ProfileLevel: %d"), static_cast<int>(propertyStatus)); }

	int32_t expectedFps = static_cast<int32_t>(expectedFrameRate_);
	CFNumberRef fpsNum = CFNumberCreate(kCFAllocatorDefault, kCFNumberSInt32Type, &expectedFps);
	propertyStatus = VTSessionSetProperty(compressionSession_, kVTCompressionPropertyKey_ExpectedFrameRate, fpsNum);
	CFRelease(fpsNum);
	if (propertyStatus != noErr) { UE_LOG(LogTemp, Warning, TEXT("VT: Failed to set ExpectedFrameRate: %d"), static_cast<int>(propertyStatus)); }

	const OSStatus prepareStatus = VTCompressionSessionPrepareToEncodeFrames(compressionSession_);
	if (prepareStatus != noErr)
	{
		UE_LOG(LogTemp, Warning, TEXT("VT: PrepareToEncodeFrames failed: %d"), static_cast<int>(prepareStatus));
	}

	CFTypeRef usingHardwareValue = nullptr;
	if (VTSessionCopyProperty(compressionSession_, kVTCompressionPropertyKey_UsingHardwareAcceleratedVideoEncoder, kCFAllocatorDefault, &usingHardwareValue) == noErr && usingHardwareValue)
	{
		if (CFGetTypeID(usingHardwareValue) == CFBooleanGetTypeID())
		{
			usingHardwareVideoToolbox_ = CFBooleanGetValue(static_cast<CFBooleanRef>(usingHardwareValue));
		}
		CFRelease(usingHardwareValue);
	}

	if (requireHardware && !usingHardwareVideoToolbox_)
	{
		UE_LOG(LogTemp, Warning, TEXT("HevcEncoder: Auto backend requested hardware VT but session is not hardware-backed"));
		ShutdownVideoToolbox();
		return false;
	}

	CFTypeRef pixelBufferAttributesValue = nullptr;
	if (VTSessionCopyProperty(compressionSession_, kVTCompressionPropertyKey_VideoEncoderPixelBufferAttributes, kCFAllocatorDefault, &pixelBufferAttributesValue) == noErr && pixelBufferAttributesValue)
	{
		if (CFGetTypeID(pixelBufferAttributesValue) == CFDictionaryGetTypeID())
		{
			vtPixelBufferAttributes_ = CFDictionaryCreateCopy(kCFAllocatorDefault, static_cast<CFDictionaryRef>(pixelBufferAttributesValue));
		}
		CFRelease(pixelBufferAttributesValue);
	}

	if (!vtPixelBufferAttributes_)
	{
		CFMutableDictionaryRef fallbackAttrs = CFDictionaryCreateMutable(
			kCFAllocatorDefault,
			4,
			&kCFTypeDictionaryKeyCallBacks,
			&kCFTypeDictionaryValueCallBacks);
		CFNumberRef widthFallback = CFNumberCreate(kCFAllocatorDefault, kCFNumberSInt32Type, &frameWidth_);
		CFNumberRef heightFallback = CFNumberCreate(kCFAllocatorDefault, kCFNumberSInt32Type, &frameHeight_);
		CFNumberRef pixelFormatNum = CFNumberCreate(kCFAllocatorDefault, kCFNumberSInt32Type, &vtPixelFormat_);
		CFMutableDictionaryRef fallbackIoSurface = CFDictionaryCreateMutable(kCFAllocatorDefault, 0, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
		CFDictionarySetValue(fallbackAttrs, kCVPixelBufferWidthKey, widthFallback);
		CFDictionarySetValue(fallbackAttrs, kCVPixelBufferHeightKey, heightFallback);
		CFDictionarySetValue(fallbackAttrs, kCVPixelBufferPixelFormatTypeKey, pixelFormatNum);
		CFDictionarySetValue(fallbackAttrs, kCVPixelBufferIOSurfacePropertiesKey, fallbackIoSurface);
		CFRelease(widthFallback);
		CFRelease(heightFallback);
		CFRelease(pixelFormatNum);
		CFRelease(fallbackIoSurface);
		vtPixelBufferAttributes_ = fallbackAttrs;
	}

	if (vtPixelBufferAttributes_)
	{
		CFTypeRef pixelFormatValue = CFDictionaryGetValue(vtPixelBufferAttributes_, kCVPixelBufferPixelFormatTypeKey);
		if (pixelFormatValue)
		{
			if (CFGetTypeID(pixelFormatValue) == CFNumberGetTypeID())
			{
				CFNumberGetValue(static_cast<CFNumberRef>(pixelFormatValue), kCFNumberSInt32Type, &vtPixelFormat_);
			}
			else if (CFGetTypeID(pixelFormatValue) == CFArrayGetTypeID() && CFArrayGetCount(static_cast<CFArrayRef>(pixelFormatValue)) > 0)
			{
				CFTypeRef firstFormatValue = CFArrayGetValueAtIndex(static_cast<CFArrayRef>(pixelFormatValue), 0);
				if (firstFormatValue && CFGetTypeID(firstFormatValue) == CFNumberGetTypeID())
				{
					CFNumberGetValue(static_cast<CFNumberRef>(firstFormatValue), kCFNumberSInt32Type, &vtPixelFormat_);
				}
			}
		}
	}

	if (!EnsureVideoToolboxPixelBufferPool())
	{
		ShutdownVideoToolbox();
		return false;
	}

	useVideoToolbox_ = true;
	selectedBackend_ = EHevcEncoderBackend::VideoToolbox;
	UE_LOG(
		LogTemp,
		Log,
		TEXT("HevcEncoder: Using VideoToolbox backend (hardware=%d, pixelFormat=%s, bitrate=%.2f Mbps, gop=%u, fps=%u)"),
		usingHardwareVideoToolbox_ ? 1 : 0,
		*PixelFormatToString(vtPixelFormat_),
		targetBitrateMbps_,
		maxKeyFrameInterval_,
		expectedFrameRate_);
	return true;
#endif
}

void HevcEncoder::ShutdownVideoToolbox()
{
	VideoToolboxCallbackState* callbackState = callbackState_;
	if (callbackState)
	{
		std::lock_guard<std::mutex> callbackLock(callbackState->Mutex);
		callbackState->Encoder = nullptr;
	}

	if (compressionSession_)
	{
		VTCompressionSessionCompleteFrames(compressionSession_, kCMTimeInvalid);
		VTCompressionSessionInvalidate(compressionSession_);
		CFRelease(compressionSession_);
		compressionSession_ = nullptr;
	}

	if (callbackState)
	{
		std::unique_lock<std::mutex> callbackLock(callbackState->Mutex);
		const bool drained = callbackState->Cv.wait_for(
			callbackLock,
			std::chrono::milliseconds(500),
			[callbackState] { return callbackState->ActiveCallbacks == 0; });
		if (!drained)
		{
			UE_LOG(LogTemp, Warning, TEXT("HevcEncoder: Timed out waiting for VideoToolbox callbacks to drain"));
		}
		callbackLock.unlock();
		delete callbackState;
		callbackState_ = nullptr;
	}

	{
		std::lock_guard<std::mutex> lock(encodeMutex_);
		completedFrames_.clear();
		pendingEncodeCount_ = 0;
	}

	if (pixelBufferPool_ && ownPixelBufferPool_)
	{
		CVPixelBufferPoolRelease(pixelBufferPool_);
	}
	pixelBufferPool_ = nullptr;
	ownPixelBufferPool_ = false;

	if (vtPixelBufferAttributes_)
	{
		CFRelease(vtPixelBufferAttributes_);
		vtPixelBufferAttributes_ = nullptr;
	}

	useVideoToolbox_ = false;
	usingHardwareVideoToolbox_ = false;
}

bool HevcEncoder::CreateFallbackPixelBufferPool()
{
	CFMutableDictionaryRef attrs = vtPixelBufferAttributes_
		? CFDictionaryCreateMutableCopy(kCFAllocatorDefault, 0, vtPixelBufferAttributes_)
		: CFDictionaryCreateMutable(kCFAllocatorDefault, 4, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
	if (!attrs)
	{
		return false;
	}

	CFNumberRef widthNum = CFNumberCreate(kCFAllocatorDefault, kCFNumberSInt32Type, &frameWidth_);
	CFNumberRef heightNum = CFNumberCreate(kCFAllocatorDefault, kCFNumberSInt32Type, &frameHeight_);
	CFNumberRef pixelFormatNum = CFNumberCreate(kCFAllocatorDefault, kCFNumberSInt32Type, &vtPixelFormat_);
	CFMutableDictionaryRef ioSurfProps = CFDictionaryCreateMutable(kCFAllocatorDefault, 0, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
	CFDictionarySetValue(attrs, kCVPixelBufferWidthKey, widthNum);
	CFDictionarySetValue(attrs, kCVPixelBufferHeightKey, heightNum);
	CFDictionarySetValue(attrs, kCVPixelBufferPixelFormatTypeKey, pixelFormatNum);
	CFDictionarySetValue(attrs, kCVPixelBufferIOSurfacePropertiesKey, ioSurfProps);
	CFRelease(widthNum);
	CFRelease(heightNum);
	CFRelease(pixelFormatNum);
	CFRelease(ioSurfProps);

	CFMutableDictionaryRef poolOptions = CFDictionaryCreateMutable(kCFAllocatorDefault, 1, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
	int32_t minimumBuffers = static_cast<int32_t>(maxPendingFrames_ + 1);
	CFNumberRef minimumBuffersNum = CFNumberCreate(kCFAllocatorDefault, kCFNumberSInt32Type, &minimumBuffers);
	CFDictionarySetValue(poolOptions, kCVPixelBufferPoolMinimumBufferCountKey, minimumBuffersNum);

	const CVReturn poolStatus = CVPixelBufferPoolCreate(kCFAllocatorDefault, poolOptions, attrs, &pixelBufferPool_);
	CFRelease(minimumBuffersNum);
	CFRelease(poolOptions);
	CFRelease(attrs);

	if (poolStatus != kCVReturnSuccess || !pixelBufferPool_)
	{
		UE_LOG(LogTemp, Warning, TEXT("HevcEncoder: Failed to create fallback pixel buffer pool status=%d (%s)"), static_cast<int>(poolStatus), DecodeCVStatus(poolStatus));
		pixelBufferPool_ = nullptr;
		return false;
	}

	ownPixelBufferPool_ = true;
	for (uint32_t i = 0; i < maxPendingFrames_; ++i)
	{
		CVPixelBufferRef warmBuffer = nullptr;
		if (CVPixelBufferPoolCreatePixelBuffer(kCFAllocatorDefault, pixelBufferPool_, &warmBuffer) != kCVReturnSuccess)
		{
			break;
		}
		CVPixelBufferRelease(warmBuffer);
	}

	return true;
}

bool HevcEncoder::EnsureVideoToolboxPixelBufferPool()
{
	if (pixelBufferPool_)
	{
		return true;
	}

	pixelBufferPool_ = VTCompressionSessionGetPixelBufferPool(compressionSession_);
	if (pixelBufferPool_)
	{
		ownPixelBufferPool_ = false;
		return true;
	}

	return CreateFallbackPixelBufferPool();
}

bool HevcEncoder::ConvertInputToPixelBuffer(const uint8_t* inputData, uint32_t inputSize, const std::string& inputFormat, CVPixelBufferRef pixelBuffer)
{
	if (!inputData || !pixelBuffer)
	{
		return false;
	}

	const bool isRGBA = (inputFormat == "rgba");
	const bool isRGBInput = (inputFormat == "bgra" || inputFormat == "rgba");
	const bool isYuvInput = (inputFormat == "yuv420");
	const uint32_t expectedRgbSize = frameWidth_ * frameHeight_ * 4;
	const uint32_t expectedYuvSize = frameWidth_ * frameHeight_ * 3 / 2;
	if ((isRGBInput && inputSize < expectedRgbSize) || (isYuvInput && inputSize < expectedYuvSize))
	{
		return false;
	}

	const CVReturn lockStatus = CVPixelBufferLockBaseAddress(pixelBuffer, 0);
	if (lockStatus != kCVReturnSuccess)
	{
		return false;
	}

	const OSType fmt = CVPixelBufferGetPixelFormatType(pixelBuffer);
	bool ok = true;

	if (isYuvInput)
	{
		const uint8_t* srcY = inputData;
		const uint8_t* srcU = srcY + (frameWidth_ * frameHeight_);
		const uint8_t* srcV = srcU + (frameWidth_ * frameHeight_ / 4);

		if (fmt == kCVPixelFormatType_420YpCbCr8BiPlanarFullRange || fmt == kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange)
		{
			uint8_t* yPlane = static_cast<uint8_t*>(CVPixelBufferGetBaseAddressOfPlane(pixelBuffer, 0));
			uint8_t* uvPlane = static_cast<uint8_t*>(CVPixelBufferGetBaseAddressOfPlane(pixelBuffer, 1));
			size_t yStride = CVPixelBufferGetBytesPerRowOfPlane(pixelBuffer, 0);
			size_t uvStride = CVPixelBufferGetBytesPerRowOfPlane(pixelBuffer, 1);

			for (uint32_t row = 0; row < frameHeight_; ++row)
			{
				memcpy(yPlane + row * yStride, srcY + row * frameWidth_, frameWidth_);
			}
			for (uint32_t row = 0; row < frameHeight_ / 2; ++row)
			{
				uint8_t* dst = uvPlane + row * uvStride;
				const uint8_t* uRow = srcU + row * (frameWidth_ / 2);
				const uint8_t* vRow = srcV + row * (frameWidth_ / 2);
				for (uint32_t col = 0; col < frameWidth_ / 2; ++col)
				{
					dst[2 * col + 0] = uRow[col];
					dst[2 * col + 1] = vRow[col];
				}
			}
		}
		else if (fmt == kCVPixelFormatType_420YpCbCr8Planar
#ifdef kCVPixelFormatType_420YpCbCr8PlanarFullRange
			|| fmt == kCVPixelFormatType_420YpCbCr8PlanarFullRange
#endif
		)
		{
			uint8_t* yPlane = static_cast<uint8_t*>(CVPixelBufferGetBaseAddressOfPlane(pixelBuffer, 0));
			uint8_t* uPlane = static_cast<uint8_t*>(CVPixelBufferGetBaseAddressOfPlane(pixelBuffer, 1));
			uint8_t* vPlane = static_cast<uint8_t*>(CVPixelBufferGetBaseAddressOfPlane(pixelBuffer, 2));
			size_t yStride = CVPixelBufferGetBytesPerRowOfPlane(pixelBuffer, 0);
			size_t uStride = CVPixelBufferGetBytesPerRowOfPlane(pixelBuffer, 1);
			size_t vStride = CVPixelBufferGetBytesPerRowOfPlane(pixelBuffer, 2);

			for (uint32_t row = 0; row < frameHeight_; ++row)
			{
				memcpy(yPlane + (row * yStride), srcY + (row * frameWidth_), frameWidth_);
			}
			for (uint32_t row = 0; row < frameHeight_ / 2; ++row)
			{
				memcpy(uPlane + (row * uStride), srcU + (row * frameWidth_ / 2), frameWidth_ / 2);
				memcpy(vPlane + (row * vStride), srcV + (row * frameWidth_ / 2), frameWidth_ / 2);
			}
		}
		else
		{
			ok = false;
		}
	}
	else if (isRGBInput)
	{
		if (fmt == kCVPixelFormatType_32BGRA)
		{
			uint8_t* dst = static_cast<uint8_t*>(CVPixelBufferGetBaseAddress(pixelBuffer));
			const size_t dstStride = CVPixelBufferGetBytesPerRow(pixelBuffer);
			for (uint32_t row = 0; row < frameHeight_; ++row)
			{
				const uint8_t* srcRow = inputData + (row * frameWidth_ * 4);
				uint8_t* dstRow = dst + row * dstStride;
				if (!isRGBA)
				{
					memcpy(dstRow, srcRow, frameWidth_ * 4);
				}
				else
				{
					for (uint32_t col = 0; col < frameWidth_; ++col)
					{
						const uint8_t* srcPixel = srcRow + (col * 4);
						uint8_t* dstPixel = dstRow + (col * 4);
						dstPixel[0] = srcPixel[2];
						dstPixel[1] = srcPixel[1];
						dstPixel[2] = srcPixel[0];
						dstPixel[3] = srcPixel[3];
					}
				}
			}
		}
		else if (fmt == kCVPixelFormatType_420YpCbCr8BiPlanarFullRange || fmt == kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange)
		{
			uint8_t* yPlane = static_cast<uint8_t*>(CVPixelBufferGetBaseAddressOfPlane(pixelBuffer, 0));
			uint8_t* uvPlane = static_cast<uint8_t*>(CVPixelBufferGetBaseAddressOfPlane(pixelBuffer, 1));
			size_t yStride = CVPixelBufferGetBytesPerRowOfPlane(pixelBuffer, 0);
			size_t uvStride = CVPixelBufferGetBytesPerRowOfPlane(pixelBuffer, 1);

			for (uint32_t row = 0; row < frameHeight_; ++row)
			{
				const uint8_t* srcRow = inputData + (row * frameWidth_ * 4);
				uint8_t* yRow = yPlane + row * yStride;
				for (uint32_t col = 0; col < frameWidth_; ++col)
				{
					uint8_t r, g, b;
					LoadRgb(srcRow + (col * 4), isRGBA, r, g, b);
					yRow[col] = RgbToY(r, g, b);
				}
			}

			for (uint32_t row = 0; row < frameHeight_; row += 2)
			{
				const uint8_t* row0 = inputData + (row * frameWidth_ * 4);
				const uint8_t* row1 = inputData + (std::min(row + 1, frameHeight_ - 1) * frameWidth_ * 4);
				uint8_t* uvRow = uvPlane + (row / 2) * uvStride;
				for (uint32_t col = 0; col < frameWidth_; col += 2)
				{
					const uint32_t col1 = std::min(col + 1, frameWidth_ - 1);
					uint8_t r00, g00, b00, r01, g01, b01, r10, g10, b10, r11, g11, b11;
					LoadRgb(row0 + (col * 4), isRGBA, r00, g00, b00);
					LoadRgb(row0 + (col1 * 4), isRGBA, r01, g01, b01);
					LoadRgb(row1 + (col * 4), isRGBA, r10, g10, b10);
					LoadRgb(row1 + (col1 * 4), isRGBA, r11, g11, b11);
					const uint8_t r = static_cast<uint8_t>((r00 + r01 + r10 + r11 + 2) >> 2);
					const uint8_t g = static_cast<uint8_t>((g00 + g01 + g10 + g11 + 2) >> 2);
					const uint8_t b = static_cast<uint8_t>((b00 + b01 + b10 + b11 + 2) >> 2);
					uvRow[col] = RgbToU(r, g, b);
					uvRow[col + 1] = RgbToV(r, g, b);
				}
			}
		}
		else if (fmt == kCVPixelFormatType_420YpCbCr8Planar
#ifdef kCVPixelFormatType_420YpCbCr8PlanarFullRange
			|| fmt == kCVPixelFormatType_420YpCbCr8PlanarFullRange
#endif
		)
		{
			uint8_t* yPlane = static_cast<uint8_t*>(CVPixelBufferGetBaseAddressOfPlane(pixelBuffer, 0));
			uint8_t* uPlane = static_cast<uint8_t*>(CVPixelBufferGetBaseAddressOfPlane(pixelBuffer, 1));
			uint8_t* vPlane = static_cast<uint8_t*>(CVPixelBufferGetBaseAddressOfPlane(pixelBuffer, 2));
			size_t yStride = CVPixelBufferGetBytesPerRowOfPlane(pixelBuffer, 0);
			size_t uStride = CVPixelBufferGetBytesPerRowOfPlane(pixelBuffer, 1);
			size_t vStride = CVPixelBufferGetBytesPerRowOfPlane(pixelBuffer, 2);

			for (uint32_t row = 0; row < frameHeight_; ++row)
			{
				const uint8_t* srcRow = inputData + (row * frameWidth_ * 4);
				uint8_t* yRow = yPlane + row * yStride;
				for (uint32_t col = 0; col < frameWidth_; ++col)
				{
					uint8_t r, g, b;
					LoadRgb(srcRow + (col * 4), isRGBA, r, g, b);
					yRow[col] = RgbToY(r, g, b);
				}
			}

			for (uint32_t row = 0; row < frameHeight_; row += 2)
			{
				const uint8_t* row0 = inputData + (row * frameWidth_ * 4);
				const uint8_t* row1 = inputData + (std::min(row + 1, frameHeight_ - 1) * frameWidth_ * 4);
				uint8_t* uRow = uPlane + (row / 2) * uStride;
				uint8_t* vRow = vPlane + (row / 2) * vStride;
				for (uint32_t col = 0; col < frameWidth_; col += 2)
				{
					const uint32_t col1 = std::min(col + 1, frameWidth_ - 1);
					uint8_t r00, g00, b00, r01, g01, b01, r10, g10, b10, r11, g11, b11;
					LoadRgb(row0 + (col * 4), isRGBA, r00, g00, b00);
					LoadRgb(row0 + (col1 * 4), isRGBA, r01, g01, b01);
					LoadRgb(row1 + (col * 4), isRGBA, r10, g10, b10);
					LoadRgb(row1 + (col1 * 4), isRGBA, r11, g11, b11);
					const uint8_t r = static_cast<uint8_t>((r00 + r01 + r10 + r11 + 2) >> 2);
					const uint8_t g = static_cast<uint8_t>((g00 + g01 + g10 + g11 + 2) >> 2);
					const uint8_t b = static_cast<uint8_t>((b00 + b01 + b10 + b11 + 2) >> 2);
					uRow[col / 2] = RgbToU(r, g, b);
					vRow[col / 2] = RgbToV(r, g, b);
				}
			}
		}
		else
		{
			ok = false;
		}
	}
	else
	{
		ok = false;
	}

	CVPixelBufferUnlockBaseAddress(pixelBuffer, 0);
	if (!ok)
	{
		UE_LOG(LogTemp, Error, TEXT("HevcEncoder: Unsupported pixel conversion path input=%s pixelFormat=%s"), *FString(inputFormat.c_str()), *PixelFormatToString(fmt));
	}
	return ok;
}

bool HevcEncoder::EncodeWithVideoToolbox(const uint8_t* inputData, uint32_t inputSize, const std::string& inputFormat)
{
	if (!useVideoToolbox_ || !compressionSession_ || !EnsureVideoToolboxPixelBufferPool())
	{
		return false;
	}

	{
		std::lock_guard<std::mutex> lock(encodeMutex_);
		if (pendingEncodeCount_ >= maxPendingFrames_)
		{
			++droppedFrames_;
			return false;
		}
		++pendingEncodeCount_;
		++perfFramesAccumulated_;
	}

	auto allocStart = std::chrono::high_resolution_clock::now();
	CVPixelBufferRef pixelBuffer = nullptr;
	const CVReturn createStatus = CVPixelBufferPoolCreatePixelBuffer(kCFAllocatorDefault, pixelBufferPool_, &pixelBuffer);
	auto allocEnd = std::chrono::high_resolution_clock::now();
	{
		std::lock_guard<std::mutex> lock(encodeMutex_);
		++vtAllocSamples_;
		vtAllocMsTotal_ += std::chrono::duration<double, std::milli>(allocEnd - allocStart).count();
	}

	if (createStatus != kCVReturnSuccess || !pixelBuffer)
	{
		std::lock_guard<std::mutex> lock(encodeMutex_);
		if (pendingEncodeCount_ > 0)
		{
			--pendingEncodeCount_;
		}
		++droppedFrames_;
		UE_LOG(LogTemp, Warning, TEXT("HevcEncoder: CVPixelBufferPoolCreatePixelBuffer failed status=%d (%s)"), static_cast<int>(createStatus), DecodeCVStatus(createStatus));
		return false;
	}

	auto convertStart = std::chrono::high_resolution_clock::now();
	const bool converted = ConvertInputToPixelBuffer(inputData, inputSize, inputFormat, pixelBuffer);
	auto convertEnd = std::chrono::high_resolution_clock::now();
	{
		std::lock_guard<std::mutex> lock(encodeMutex_);
		++vtConvertSamples_;
		vtConvertMsTotal_ += std::chrono::duration<double, std::milli>(convertEnd - convertStart).count();
	}

	if (!converted)
	{
		CVPixelBufferRelease(pixelBuffer);
		std::lock_guard<std::mutex> lock(encodeMutex_);
		if (pendingEncodeCount_ > 0)
		{
			--pendingEncodeCount_;
		}
		++droppedFrames_;
		return false;
	}

	const bool forceKeyframe = (frameCounter_ == 0);
	CFDictionaryRef frameProperties = nullptr;
	if (forceKeyframe)
	{
		CFStringRef keys[] = { kVTEncodeFrameOptionKey_ForceKeyFrame };
		CFBooleanRef values[] = { kCFBooleanTrue };
		frameProperties = CFDictionaryCreate(
			kCFAllocatorDefault,
			reinterpret_cast<const void**>(keys),
			reinterpret_cast<const void**>(values),
			1,
			&kCFTypeDictionaryKeyCallBacks,
			&kCFTypeDictionaryValueCallBacks);
	}

	std::unique_ptr<PendingFrameContext> frameContext(new PendingFrameContext());
	frameContext->Encoder = this;
	frameContext->FrameIndex = frameCounter_;
	frameContext->SubmitTime = std::chrono::high_resolution_clock::now();
	frameContext->ForceKeyframe = forceKeyframe;

	const CMTime presentationTime = CMTimeMake(frameCounter_, expectedFrameRate_);
	const CMTime duration = CMTimeMake(1, expectedFrameRate_);
	++frameCounter_;

	VTEncodeInfoFlags encodeInfoFlags = 0;
	const OSStatus encodeStatus = VTCompressionSessionEncodeFrame(
		compressionSession_,
		pixelBuffer,
		presentationTime,
		duration,
		frameProperties,
		frameContext.get(),
		&encodeInfoFlags);

	if (frameProperties)
	{
		CFRelease(frameProperties);
	}
	CVPixelBufferRelease(pixelBuffer);

	if (encodeStatus != noErr)
	{
		std::lock_guard<std::mutex> lock(encodeMutex_);
		if (pendingEncodeCount_ > 0)
		{
			--pendingEncodeCount_;
		}
		++droppedFrames_;
		UE_LOG(LogTemp, Warning, TEXT("HevcEncoder: VTCompressionSessionEncodeFrame failed status=%d"), static_cast<int>(encodeStatus));
		return false;
	}

	if ((encodeInfoFlags & kVTEncodeInfo_FrameDropped) != 0)
	{
		std::lock_guard<std::mutex> lock(encodeMutex_);
		if (pendingEncodeCount_ > 0)
		{
			--pendingEncodeCount_;
		}
		++droppedFrames_;
		return true;
	}

	frameContext.release();
	return true;
}

void HevcEncoder::DrainCompletedVideoToolboxFrames()
{
	EncodedFrame completedFrame;
	bool hasFrame = false;
	{
		std::lock_guard<std::mutex> lock(encodeMutex_);
		if (!completedFrames_.empty())
		{
			completedFrame = std::move(completedFrames_.front());
			completedFrames_.pop_front();
			hasFrame = true;
		}
	}

	if (hasFrame)
	{
		PublishOutputFrame(completedFrame.Data);
	}
	else
	{
		ClearOutputFrame();
	}
}

void HevcEncoder::CompressionCallback(
	void* outputCallbackRefCon,
	void* sourceFrameRefCon,
	OSStatus status,
	VTEncodeInfoFlags infoFlags,
	CMSampleBufferRef sampleBuffer)
{
	VideoToolboxCallbackState* callbackState = static_cast<VideoToolboxCallbackState*>(outputCallbackRefCon);
	PendingFrameContext* frameContext = static_cast<PendingFrameContext*>(sourceFrameRefCon);
	if (!callbackState)
	{
		delete frameContext;
		return;
	}

	HevcEncoder* encoder = nullptr;
	{
		std::lock_guard<std::mutex> callbackLock(callbackState->Mutex);
		++callbackState->ActiveCallbacks;
		encoder = callbackState->Encoder;
	}

	struct FScopedCallbackActivity
	{
		VideoToolboxCallbackState* State;
		~FScopedCallbackActivity()
		{
			std::lock_guard<std::mutex> callbackLock(State->Mutex);
			if (State->ActiveCallbacks > 0)
			{
				--State->ActiveCallbacks;
			}
			State->Cv.notify_all();
		}
	} callbackScope{ callbackState };

	if (!encoder)
	{
		delete frameContext;
		return;
	}

	if ((infoFlags & kVTEncodeInfo_FrameDropped) != 0)
	{
		std::lock_guard<std::mutex> lock(encoder->encodeMutex_);
		if (encoder->pendingEncodeCount_ > 0)
		{
			--encoder->pendingEncodeCount_;
		}
		++encoder->droppedFrames_;
		delete frameContext;
		return;
	}

	encoder->HandleEncodedFrame(status, sampleBuffer, frameContext);
}

void HevcEncoder::HandleEncodedFrame(OSStatus status, CMSampleBufferRef sampleBuffer, PendingFrameContext* frameContext)
{
	std::unique_ptr<PendingFrameContext> ownedContext(frameContext);
	std::vector<uint8_t> encodedData;
	bool isKeyframe = false;

	if (status == noErr && sampleBuffer)
	{
		CMBlockBufferRef blockBuffer = CMSampleBufferGetDataBuffer(sampleBuffer);
		CMFormatDescriptionRef formatDescription = CMSampleBufferGetFormatDescription(sampleBuffer);
		size_t parameterSetCount = 0;
		int naluHeaderLength = 4;
		if (formatDescription)
		{
			CMVideoFormatDescriptionGetHEVCParameterSetAtIndex(formatDescription, 0, nullptr, nullptr, &parameterSetCount, &naluHeaderLength);
			if (naluHeaderLength != 1 && naluHeaderLength != 2 && naluHeaderLength != 4)
			{
				naluHeaderLength = 4;
			}
		}

		CFArrayRef attachments = CMSampleBufferGetSampleAttachmentsArray(sampleBuffer, false);
		if (attachments && CFArrayGetCount(attachments) > 0)
		{
			CFDictionaryRef attachment = static_cast<CFDictionaryRef>(CFArrayGetValueAtIndex(attachments, 0));
			CFBooleanRef dependsOnOthers = static_cast<CFBooleanRef>(CFDictionaryGetValue(attachment, kCMSampleAttachmentKey_DependsOnOthers));
			isKeyframe = (dependsOnOthers == kCFBooleanFalse);
		}

		if (formatDescription)
		{
			for (size_t i = 0; i < parameterSetCount; ++i)
			{
				const uint8_t* parameterSet = nullptr;
				size_t parameterSetSize = 0;
				if (CMVideoFormatDescriptionGetHEVCParameterSetAtIndex(formatDescription, i, &parameterSet, &parameterSetSize, nullptr, nullptr) == noErr && parameterSet && parameterSetSize > 0)
				{
					encodedData.insert(encodedData.end(), { 0x00, 0x00, 0x00, 0x01 });
					encodedData.insert(encodedData.end(), parameterSet, parameterSet + parameterSetSize);
				}
			}
		}

		if (blockBuffer)
		{
			char* dataPointer = nullptr;
			size_t dataSize = 0;
			if (CMBlockBufferGetDataPointer(blockBuffer, 0, nullptr, &dataSize, &dataPointer) == noErr && dataPointer && dataSize > 0)
			{
				size_t offset = 0;
				while (offset + naluHeaderLength <= dataSize)
				{
					uint32_t naluLength = 0;
					if (naluHeaderLength == 4)
					{
						naluLength = (static_cast<uint8_t>(dataPointer[offset]) << 24) |
							(static_cast<uint8_t>(dataPointer[offset + 1]) << 16) |
							(static_cast<uint8_t>(dataPointer[offset + 2]) << 8) |
							static_cast<uint8_t>(dataPointer[offset + 3]);
					}
					else if (naluHeaderLength == 2)
					{
						naluLength = (static_cast<uint8_t>(dataPointer[offset]) << 8) |
							static_cast<uint8_t>(dataPointer[offset + 1]);
					}
					else
					{
						naluLength = static_cast<uint8_t>(dataPointer[offset]);
					}

					offset += naluHeaderLength;
					if (naluLength == 0 || offset + naluLength > dataSize)
					{
						break;
					}

					encodedData.insert(encodedData.end(), { 0x00, 0x00, 0x00, 0x01 });
					encodedData.insert(
						encodedData.end(),
						reinterpret_cast<uint8_t*>(dataPointer + offset),
						reinterpret_cast<uint8_t*>(dataPointer + offset + naluLength));
					offset += naluLength;
				}
			}
		}
	}

	const double encodeMilliseconds = ownedContext
		? std::chrono::duration<double, std::milli>(std::chrono::high_resolution_clock::now() - ownedContext->SubmitTime).count()
		: 0.0;

	std::lock_guard<std::mutex> lock(encodeMutex_);
	if (pendingEncodeCount_ > 0)
	{
		--pendingEncodeCount_;
	}

	if (status == noErr && !encodedData.empty())
	{
		++vtEncodeSamples_;
		vtEncodeMsTotal_ += encodeMilliseconds;
		if (completedFrames_.size() >= maxPendingFrames_)
		{
			completedFrames_.pop_front();
			++droppedFrames_;
			++droppedCompletedFrames_;
		}

		completedFrames_.push_back({ std::move(encodedData), encodeMilliseconds, isKeyframe });
	}
	else
	{
		++droppedFrames_;
	}
}
#endif // CITHRUS_VIDEOTOOLBOX_AVAILABLE

#ifdef CITHRUS_VIDEOTOOLBOX_AVAILABLE
static const TCHAR* DecodeCVStatus(OSStatus status)
{
	switch (status)
	{
	case kCVReturnSuccess: return TEXT("kCVReturnSuccess");
	case kCVReturnInvalidArgument: return TEXT("kCVReturnInvalidArgument");
	case kCVReturnAllocationFailed: return TEXT("kCVReturnAllocationFailed");
	case kCVReturnUnsupported: return TEXT("kCVReturnUnsupported");
	case kCVReturnInvalidPixelFormat: return TEXT("kCVReturnInvalidPixelFormat");
	case kCVReturnInvalidSize: return TEXT("kCVReturnInvalidSize");
	case kCVReturnInvalidPixelBufferAttributes: return TEXT("kCVReturnInvalidPixelBufferAttributes");
	case kCVReturnPixelBufferNotOpenGLCompatible: return TEXT("kCVReturnPixelBufferNotOpenGLCompatible");
	case kCVReturnWouldExceedAllocationThreshold: return TEXT("kCVReturnWouldExceedAllocationThreshold");
	case kCVReturnPoolAllocationFailed: return TEXT("kCVReturnPoolAllocationFailed");
	default: return TEXT("Unknown CV status");
	}
}
#endif
