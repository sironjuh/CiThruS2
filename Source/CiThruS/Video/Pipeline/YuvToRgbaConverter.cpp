#include "YuvToRgbaConverter.h"

#ifdef CITHRUS_SSE41_AVAILABLE
#include <emmintrin.h>
#include <pmmintrin.h>
#include <smmintrin.h>
#endif // CITHRUS_SSE41_AVAILABLE

#if defined(CITHRUS_NEON_AVAILABLE)
#include <arm_neon.h>
#endif // CITHRUS_NEON_AVAILABLE

#include <algorithm>
#include <array>
#include <cstdint>
#include <stdexcept>

namespace
{
inline uint8_t ClampByte(const int value)
{
    if (value < 0)
    {
        return 0;
    }

    if (value > 255)
    {
        return 255;
    }

    return static_cast<uint8_t>(value);
}

#if defined(CITHRUS_NEON_AVAILABLE)
inline uint8x8_t PackClampedBytes(const int32x4_t lowValues, const int32x4_t highValues)
{
    return vqmovun_s16(vcombine_s16(vqmovn_s32(lowValues), vqmovn_s32(highValues)));
}

inline void StoreYuvBlockAsRgba(
    const uint8x8_t yValues,
    const int16x8_t uValues,
    const int16x8_t vValues,
    const bool bgra,
    uint8_t* output)
{
    const int16x8_t y16 = vreinterpretq_s16_u16(vmovl_u8(yValues));

    const int32x4_t yLow = vmovl_s16(vget_low_s16(y16));
    const int32x4_t yHigh = vmovl_s16(vget_high_s16(y16));
    const int32x4_t uLow = vmovl_s16(vget_low_s16(uValues));
    const int32x4_t uHigh = vmovl_s16(vget_high_s16(uValues));
    const int32x4_t vLow = vmovl_s16(vget_low_s16(vValues));
    const int32x4_t vHigh = vmovl_s16(vget_high_s16(vValues));

    const int32x4_t rLow = vaddq_s32(yLow, vshrq_n_s32(vmulq_n_s32(vLow, 91881), 16));
    const int32x4_t rHigh = vaddq_s32(yHigh, vshrq_n_s32(vmulq_n_s32(vHigh, 91881), 16));
    const int32x4_t gLow = vsubq_s32(yLow, vshrq_n_s32(vaddq_s32(vmulq_n_s32(uLow, 22554), vmulq_n_s32(vLow, 46802)), 16));
    const int32x4_t gHigh = vsubq_s32(yHigh, vshrq_n_s32(vaddq_s32(vmulq_n_s32(uHigh, 22554), vmulq_n_s32(vHigh, 46802)), 16));
    const int32x4_t bLow = vaddq_s32(yLow, vshrq_n_s32(vmulq_n_s32(uLow, 116130), 16));
    const int32x4_t bHigh = vaddq_s32(yHigh, vshrq_n_s32(vmulq_n_s32(uHigh, 116130), 16));

    const uint8x8_t rBytes = PackClampedBytes(rLow, rHigh);
    const uint8x8_t gBytes = PackClampedBytes(gLow, gHigh);
    const uint8x8_t bBytes = PackClampedBytes(bLow, bHigh);

    uint8x8x4_t rgba;
    if (bgra)
    {
        rgba.val[0] = bBytes;
        rgba.val[1] = gBytes;
        rgba.val[2] = rBytes;
    }
    else
    {
        rgba.val[0] = rBytes;
        rgba.val[1] = gBytes;
        rgba.val[2] = bBytes;
    }

    rgba.val[3] = vdup_n_u8(255);
    vst4_u8(output, rgba);
}
#endif // CITHRUS_NEON_AVAILABLE
}

YuvToRgbaConverter::YuvToRgbaConverter(const uint16_t& frameWidth, const uint16_t& frameHeight, const std::string& format)
    : outputFrameWidth_(frameWidth), outputFrameHeight_(frameHeight)
{
    uint32_t outputSize = outputFrameWidth_ * outputFrameHeight_ * 4;
    outputData_ = new uint8_t[outputSize];

    if (format != "rgba" && format != "bgra")
    {
        throw std::invalid_argument("Unsupported output format: " + format);
    }

    GetInputPin<0>().SetAcceptedFormat("yuv420");
    GetOutputPin<0>().SetFormat(format);

    GetOutputPin<0>().SetData(outputData_);
    GetOutputPin<0>().SetSize(outputSize);
}

YuvToRgbaConverter::~YuvToRgbaConverter()
{
	delete[] outputData_;
	outputData_ = nullptr;

    GetOutputPin<0>().SetData(nullptr);
    GetOutputPin<0>().SetSize(0);
}

void YuvToRgbaConverter::Process()
{
    const uint8_t* inputData = GetInputPin<0>().GetData();
    size_t inputSize = GetInputPin<0>().GetSize();
    const size_t expectedInputSize = outputFrameWidth_ * outputFrameHeight_ * 3 / 2;

    if (!inputData || inputSize == 0)
    {
        GetOutputPin<0>().SetData(nullptr);
        GetOutputPin<0>().SetSize(0);
        return;
    }

    if (inputSize != expectedInputSize)
    {
        static uint32_t sizeMismatchLogCount = 0;
        if (sizeMismatchLogCount < 10)
        {
            UE_LOG(
                LogTemp,
                Warning,
                TEXT("YuvToRgbaConverter: input size mismatch, got=%u expected=%u (w=%u h=%u)"),
                static_cast<uint32_t>(inputSize),
                static_cast<uint32_t>(expectedInputSize),
                static_cast<uint32_t>(outputFrameWidth_),
                static_cast<uint32_t>(outputFrameHeight_));
            sizeMismatchLogCount++;
        }

        GetOutputPin<0>().SetData(nullptr);
        GetOutputPin<0>().SetSize(0);
        return;
    }

#if defined(CITHRUS_NEON_AVAILABLE)
    static bool loggedNeonPath = false;
    if (!loggedNeonPath)
    {
        UE_LOG(LogTemp, Display, TEXT("YuvToRgbaConverter: Using NEON optimized path (ARM)."));
        loggedNeonPath = true;
    }

    YuvToRgbaNeon(inputData, outputData_, outputFrameWidth_, outputFrameHeight_);
#elif defined(CITHRUS_SSE41_AVAILABLE)
    YuvToRgbaSse41(inputData, &outputData_, outputFrameWidth_, outputFrameHeight_);
#else
    static bool loggedScalarFallback = false;
    if (!loggedScalarFallback)
    {
        UE_LOG(LogTemp, Log, TEXT("YuvToRgbaConverter: SIMD unavailable, using scalar fallback."));
        loggedScalarFallback = true;
    }

    YuvToRgbaScalar(inputData, outputData_, outputFrameWidth_, outputFrameHeight_);
#endif // CITHRUS_NEON_AVAILABLE

    GetOutputPin<0>().SetData(outputData_);
    GetOutputPin<0>().SetSize(outputFrameWidth_ * outputFrameHeight_ * 4);
}

void YuvToRgbaConverter::YuvToRgbaSse41(const uint8_t* input, uint8_t** output, int width, int height)
{
    // TODO: Something is not right here. The result has a slight greenish tint

#ifdef CITHRUS_SSE41_AVAILABLE
    // This efficiently converts pixels from YUV 4:2:0 to RGBA by using SSE 4.1 instructions to process multiple values simultaneously

    const int mini[4] =   {   0,   0,   0,   0 };
    const int middle[4] = { 128, 128, 128, 128 };
    const int maxi[4] =   { 255, 255, 255, 255 };

    const __m128i min_val = _mm_loadu_si128((__m128i const*)mini);
    const __m128i middle_val = _mm_loadu_si128((__m128i const*)middle);
    const __m128i max_val = _mm_loadu_si128((__m128i const*)maxi);

    uint8_t* row_r = (uint8_t*)malloc(width * 4);
    uint8_t* row_g = (uint8_t*)malloc(width * 4);
    uint8_t* row_b = (uint8_t*)malloc(width * 4);

    const uint8_t* in_y = &input[0];
    const uint8_t* in_u = &input[width * height];
    const uint8_t* in_v = &input[width * height + (width * height >> 2)];
    uint8_t* out = *output;

    int8_t row = 0;
    int32_t pix = 0;

    __m128i luma_shufflemask = _mm_set_epi8(-1, -1, -1, 3, -1, -1, -1, 2, -1, -1, -1, 1, -1, -1, -1, 0);
    __m128i chroma_shufflemask = _mm_set_epi8(-1, -1, -1, 1, -1, -1, -1, 1, -1, -1, -1, 0, -1, -1, -1, 0);

    int shift_r;
    int shift_g;
    int shift_b;

    std::string outputFormat = GetOutputPin<0>().GetFormat();

    if (outputFormat == "rgba")
    {
        shift_r = 0;
        shift_g = 8;
        shift_b = 16;
    }
    else if (outputFormat == "bgra")
    {
        shift_r = 16;
        shift_g = 8;
        shift_b = 0;
    }
    else
    {
        throw std::runtime_error("Unsupported format");
    }

    __m128i a_pix = _mm_set_epi8(-1, 0, 0, 0, -1, 0, 0, 0, -1, 0, 0, 0, -1, 0, 0, 0);

    for (int32_t i = 0; i < width * height; i += 16)
    {
        // Load 16 bytes (16 luma pixels)
        __m128i y_a = _mm_loadu_si128((__m128i const*) in_y);
        in_y += 16;

        __m128i luma_a = _mm_shuffle_epi8(y_a, luma_shufflemask);
        __m128i u_a, v_a, chroma_u, chroma_v;

        // For every second row
        if (!row)
        {
            u_a = _mm_loadl_epi64((__m128i const*) in_u);
            in_u += 8;

            v_a = _mm_loadl_epi64((__m128i const*) in_v);
            in_v += 8;

            chroma_u = _mm_shuffle_epi8(u_a, chroma_shufflemask);
            chroma_v = _mm_shuffle_epi8(v_a, chroma_shufflemask);
        }

        __m128i r_pix_temp, g_pix_temp, b_pix_temp, temp_a, temp_b;

        for (int j = 0; j < 4; j++)
        {
            // We use the same chroma for two rows
            if (row)
            {
                r_pix_temp = _mm_loadu_si128((__m128i const*) & row_r[pix * 2 + j * 16]);
                g_pix_temp = _mm_loadu_si128((__m128i const*) & row_g[pix * 2 + j * 16]);
                b_pix_temp = _mm_loadu_si128((__m128i const*) & row_b[pix * 2 + j * 16]);
            }
            else
            {
                chroma_u = _mm_sub_epi32(chroma_u, middle_val);
                chroma_v = _mm_sub_epi32(chroma_v, middle_val);

                r_pix_temp = _mm_add_epi32(chroma_v, _mm_add_epi32(_mm_srai_epi32(chroma_v, 2), _mm_add_epi32(_mm_srai_epi32(chroma_v, 3), _mm_srai_epi32(chroma_v, 5))));
                temp_a = _mm_add_epi32(_mm_srai_epi32(chroma_u, 2), _mm_add_epi32(_mm_srai_epi32(chroma_u, 4), _mm_srai_epi32(chroma_u, 5)));
                temp_b = _mm_add_epi32(_mm_srai_epi32(chroma_v, 1), _mm_add_epi32(_mm_srai_epi32(chroma_v, 3), _mm_add_epi32(_mm_srai_epi32(chroma_v, 4), _mm_srai_epi32(chroma_v, 5))));
                g_pix_temp = _mm_add_epi32(temp_a, temp_b);
                b_pix_temp = _mm_add_epi32(chroma_u, _mm_add_epi32(_mm_srai_epi32(chroma_u, 1), _mm_add_epi32(_mm_srai_epi32(chroma_u, 2), _mm_srai_epi32(chroma_u, 6))));

                // Store results to be used for the next row
                _mm_storeu_si128((__m128i*) & row_r[pix * 2 + j * 16], r_pix_temp);
                _mm_storeu_si128((__m128i*) & row_g[pix * 2 + j * 16], g_pix_temp);
                _mm_storeu_si128((__m128i*) & row_b[pix * 2 + j * 16], b_pix_temp);
            }

            __m128i r_pix = _mm_slli_epi32(_mm_max_epi32(min_val, _mm_min_epi32(max_val, _mm_add_epi32(luma_a, r_pix_temp))), shift_r);
            __m128i g_pix = _mm_slli_epi32(_mm_max_epi32(min_val, _mm_min_epi32(max_val, _mm_sub_epi32(luma_a, g_pix_temp))), shift_g);
            __m128i b_pix = _mm_slli_epi32(_mm_max_epi32(min_val, _mm_min_epi32(max_val, _mm_add_epi32(luma_a, b_pix_temp))), shift_b);

            __m128i rgba = _mm_adds_epu8(_mm_adds_epu8(r_pix, g_pix), _mm_adds_epu8(b_pix, a_pix));

            _mm_storeu_si128((__m128i*)out, rgba);
            out += 16;

            if (j != 3)
            {
                u_a = _mm_srli_si128(u_a, 2);
                v_a = _mm_srli_si128(v_a, 2);
                chroma_u = _mm_shuffle_epi8(u_a, chroma_shufflemask);
                chroma_v = _mm_shuffle_epi8(v_a, chroma_shufflemask);

                y_a = _mm_srli_si128(y_a, 4);
                luma_a = _mm_shuffle_epi8(y_a, luma_shufflemask);
            }
        }

        // Track rows for chroma
        pix += 16;
        if (pix == width)
        {
            row = !row;
            pix = 0;
        }
    }

    free(row_r);
    free(row_g);
    free(row_b);

#endif // CITHRUS_SSE41_AVAILABLE
}

void YuvToRgbaConverter::YuvToRgbaNeon(const uint8_t* input, uint8_t* output, int width, int height)
{
#if defined(CITHRUS_NEON_AVAILABLE)
    if (!input || !output || width <= 0 || height <= 0)
    {
        return;
    }

    const uint8_t* yPlane = input;
    const uint8_t* uPlane = yPlane + (width * height);
    const uint8_t* vPlane = uPlane + (width * height / 4);

    const bool bgra = (GetOutputPin<0>().GetFormat() == "bgra");
    const uint16x8_t chromaBias = vdupq_n_u16(128);

    for (int y = 0; y < height; ++y)
    {
        const uint8_t* yRow = yPlane + (y * width);
        const uint8_t* uRow = uPlane + ((y / 2) * (width / 2));
        const uint8_t* vRow = vPlane + ((y / 2) * (width / 2));
        uint8_t* outputRow = output + (y * width * 4);

        int x = 0;
        for (; x + 16 <= width; x += 16)
        {
            const uint8x16_t yValues = vld1q_u8(yRow + x);
            const uint8x8_t uSamples = vld1_u8(uRow + (x >> 1));
            const uint8x8_t vSamples = vld1_u8(vRow + (x >> 1));
            const uint8x8x2_t uExpanded = vzip_u8(uSamples, uSamples);
            const uint8x8x2_t vExpanded = vzip_u8(vSamples, vSamples);
            const int16x8_t uLow = vreinterpretq_s16_u16(vsubq_u16(vmovl_u8(uExpanded.val[0]), chromaBias));
            const int16x8_t uHigh = vreinterpretq_s16_u16(vsubq_u16(vmovl_u8(uExpanded.val[1]), chromaBias));
            const int16x8_t vLow = vreinterpretq_s16_u16(vsubq_u16(vmovl_u8(vExpanded.val[0]), chromaBias));
            const int16x8_t vHigh = vreinterpretq_s16_u16(vsubq_u16(vmovl_u8(vExpanded.val[1]), chromaBias));

            StoreYuvBlockAsRgba(vget_low_u8(yValues), uLow, vLow, bgra, outputRow + (x * 4));
            StoreYuvBlockAsRgba(vget_high_u8(yValues), uHigh, vHigh, bgra, outputRow + ((x + 8) * 4));
        }

        for (; x < width; ++x)
        {
            const int yIndex = y * width + x;
            const int uvIndex = (y / 2) * (width / 2) + (x / 2);
            const int Y = static_cast<int>(yPlane[yIndex]);
            const int U = static_cast<int>(uPlane[uvIndex]) - 128;
            const int V = static_cast<int>(vPlane[uvIndex]) - 128;
            const uint8_t R = ClampByte(Y + ((91881 * V) >> 16));
            const uint8_t G = ClampByte(Y - ((22554 * U + 46802 * V) >> 16));
            const uint8_t B = ClampByte(Y + ((116130 * U) >> 16));
            const int outIndex = yIndex * 4;

            if (bgra)
            {
                output[outIndex + 0] = B;
                output[outIndex + 1] = G;
                output[outIndex + 2] = R;
            }
            else
            {
                output[outIndex + 0] = R;
                output[outIndex + 1] = G;
                output[outIndex + 2] = B;
            }

            output[outIndex + 3] = 255;
        }
    }
#else
    YuvToRgbaScalar(input, output, width, height);
#endif // CITHRUS_NEON_AVAILABLE
}

void YuvToRgbaConverter::YuvToRgbaScalar(const uint8_t* input, uint8_t* output, int width, int height)
{
    if (!input || !output || width <= 0 || height <= 0)
    {
        return;
    }

    const uint8_t* yPlane = input;
    const uint8_t* uPlane = yPlane + (width * height);
    const uint8_t* vPlane = uPlane + (width * height / 4);

    const std::string outputFormat = GetOutputPin<0>().GetFormat();
    const bool bgra = (outputFormat == "bgra");

    for (int y = 0; y < height; ++y)
    {
        for (int x = 0; x < width; ++x)
        {
            const int yIndex = y * width + x;
            const int uvIndex = (y / 2) * (width / 2) + (x / 2);

            const int Y = static_cast<int>(yPlane[yIndex]);
            const int U = static_cast<int>(uPlane[uvIndex]) - 128;
            const int V = static_cast<int>(vPlane[uvIndex]) - 128;

            // BT.601 full-range approximation
            const int r = Y + ((91881 * V) >> 16);
            const int g = Y - ((22554 * U + 46802 * V) >> 16);
            const int b = Y + ((116130 * U) >> 16);

            const uint8_t R = ClampByte(r);
            const uint8_t G = ClampByte(g);
            const uint8_t B = ClampByte(b);

            const int outIndex = yIndex * 4;

            if (bgra)
            {
                output[outIndex + 0] = B;
                output[outIndex + 1] = G;
                output[outIndex + 2] = R;
            }
            else
            {
                output[outIndex + 0] = R;
                output[outIndex + 1] = G;
                output[outIndex + 2] = B;
            }

            output[outIndex + 3] = 255;
        }
    }
}
