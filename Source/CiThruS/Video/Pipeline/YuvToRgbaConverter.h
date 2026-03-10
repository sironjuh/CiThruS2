#pragma once

#if (defined(__ARM_NEON) || defined(__ARM_NEON__)) && (defined(__aarch64__) || defined(__arm64__))
#ifndef CITHRUS_NEON_AVAILABLE
#define CITHRUS_NEON_AVAILABLE 1
#endif // CITHRUS_NEON_AVAILABLE
#endif // defined(...)

#if ((defined(__x86_64__) || defined(_M_X64)) && !defined(_M_ARM64EC))
#ifndef CITHRUS_SSE41_AVAILABLE
#define CITHRUS_SSE41_AVAILABLE
#endif // CITHRUS_SSE41_AVAILABLE
#elif !defined(CITHRUS_NEON_AVAILABLE)
#pragma message (__FILE__ ": warning: SSE4.1/NEON instructions not available, using scalar YUV conversion")
#endif // defined(...)

#include "PipelineFilter.h"

class CITHRUS_API YuvToRgbaConverter : public PipelineFilter<1, 1>
{
public:
	YuvToRgbaConverter(const uint16_t& frameWidth, const uint16_t& frameHeight, const std::string& format = "rgba");
	virtual ~YuvToRgbaConverter();

	virtual void Process() override;

protected:
	uint8_t* outputData_;

	uint16_t outputFrameWidth_;
	uint16_t outputFrameHeight_;

	void YuvToRgbaSse41(const uint8_t* input, uint8_t** output, int width, int height);
	void YuvToRgbaNeon(const uint8_t* input, uint8_t* output, int width, int height);
	void YuvToRgbaScalar(const uint8_t* input, uint8_t* output, int width, int height);
};
