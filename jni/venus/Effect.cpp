﻿#ifdef _WIN32
#	define _USE_MATH_DEFINES  // for M_PI
#endif
#include <cmath>
#include <cassert>

#include <opencv2/imgproc.hpp>
#if TRACE_IMAGES 
#include <opencv2/highgui.hpp>
#endif

#include "venus/colorspace.h"
#include "venus/Effect.h"
#include "venus/opencv_utility.h"
#include "venus/scalar.h"


using namespace cv;

namespace venus {

// bilinear interpolation
template <typename T, int N>
cv::Vec<T, N> interpolate(const Mat& image,
	const int& x0, const int& y0, const int& x1, const int& y1,
	const float& wx, const float& wy)
{
	using Vector = cv::Vec<T, N>;

	// note at<T>(y, x) or at<T>(Point(x, y)) or at<T>(r, c)
	const Vector& c00 = image.at<Vector>(y0, x0);
	const Vector& c01 = image.at<Vector>(y0, x1);
	const Vector& c10 = image.at<Vector>(y1, x0);
	const Vector& c11 = image.at<Vector>(y1, x1);

	Vector color;
	const float l_wx = 1.0F - wx;
	const float l_wy = 1.0F - wy;
	for(int i = 0; i < N; ++i)
	{
		float c0 = c00[i] * wx + c01[i] * l_wx;
		float c1 = c10[i] * wx + c11[i] * l_wx;
		float c = c0 * wy + c1 * l_wy;

		// round off to the nearest for integer types
		if(std::is_integral<T>::value)
			c += T(0.5F);

		color[i] = static_cast<T>(c);
	}

	return color;
}

void Effect::mapColor(cv::Mat& dst, const cv::Mat&src, const uint8_t table[256])
{
	assert(src.depth() == CV_8U);
	dst.create(src.rows, src.cols, src.type());

	const int channels = src.channels();
	const int length = src.rows * src.cols * channels;

	if(channels == 4)
	{
		uint8_t* dst_data = dst.data;
		const uint8_t* src_data = src.data;

		#pragma omp parallel for
		for(int i = 0; i < length; i += 4)
		{
			dst_data[i+0] = table[src_data[i+0]];
			dst_data[i+1] = table[src_data[i+1]];
			dst_data[i+2] = table[src_data[i+2]];
			dst_data[i+3] = src_data[i+3];  // keep alpha untouched
		}
	}
	else
	{
		uint8_t* dst_data = dst.data;
		const uint8_t* src_data = src.data;

		#pragma omp parallel for
		for(int i = 0; i < length; ++i)
			dst_data[i] = table[src_data[i]];
	}
}

void Effect::mapColor(cv::Mat& dst, const cv::Mat&src, const uint8_t* mask, const uint8_t table[256])
{
	assert(src.depth() == CV_8U && mask != nullptr);
	dst.create(src.rows, src.cols, src.type());

	const int channels = src.channels();
	const int length = src.rows * src.cols * channels;

	if(channels == 4)
	{
		uint8_t* dst_data = dst.data;
		const uint8_t* src_data = src.data;

		#pragma omp parallel for
		for(int i = 0; i < length; i += 4)
		{
			int _0 = i, _1 = i + 1, _2 = i + 2, _3 = i + 3;
			dst_data[_0] = lerp(src_data[_0], table[src_data[_0]], mask[i]);
			dst_data[_1] = lerp(src_data[_1], table[src_data[_1]], mask[i]);
			dst_data[_2] = lerp(src_data[_2], table[src_data[_2]], mask[i]);
			dst_data[_3] =      src_data[_3];  // keep alpha untouched
		}
	}
	else
	{
		uint8_t* dst_data = dst.data;
		const uint8_t* src_data = src.data;

		#pragma omp parallel for
		for(int i = 0; i < length; ++i)
		{
			if(mask[i] == 0)
				dst_data[i] = src_data[i];  // copy transparent region
			else
//				dst_data[i] = lerp(src_data[i], table[src_data[i]], mask[i]/255.0F);
				dst_data[i] = lerp(src_data[i], table[src_data[i]], mask[i]);
		}
	}
}

void Effect::tone(cv::Mat& dst, const cv::Mat& src, uint32_t color, float amount)
{
	assert(src.type() == CV_8UC4 || src.type() == CV_32FC4);
	dst.create(src.size(), src.type());

	const float l_amount = 1.0F - amount;
	if(src.depth() == CV_32F)
		amount /= 255;

	Vec4f target = cast(color) * amount;
	
	switch(src.type())
	{
	case CV_8UC4:
		for(int r = 0; r < src.rows; ++r)
		for(int c = 0; c < src.cols; ++c)
		{
			const Vec4b& src_color = src.at<Vec4b>(r, c);
			Vec4b& dst_color = dst.at<Vec4b>(r, c);
			for(int i = 0; i < 3; ++i)  // loop unrolling
				dst_color[i] = static_cast<uint8_t>(src_color[i] * l_amount + target[i]);
		}
		break;
	case CV_32FC4:
		for(int r = 0; r < src.rows; ++r)
		for(int c = 0; c < src.cols; ++c)
		{
			const Vec4f& src_color = src.at<Vec4f>(r, c);
			Vec4f& dst_color = dst.at<Vec4f>(r, c);
			for(int i = 0; i < 3; ++i)  // loop unrolling
				dst_color[i] = src_color[i] * l_amount + target[i];
		}
		break;
	default:
		assert(false);
		break;
	}
}

void Effect::posterize(cv::Mat& dst, const cv::Mat& src, float level)
{
	assert(1.0F <= level && level <= 256.0F);
	const int depth = src.depth();
	const int channel = dst.channels();
	if(src.data != dst.data)
		src.copyTo(dst);

	if(depth == CV_8U)
	{
		level = 256 / level;
		uint8_t table[256];
		for(int i = 0; i < 256; ++i)
			table[i] = cvRound(std::floor(i / level) * level);
	
		mapColor(dst, src, table);
	}
	else if(depth == CV_32F)
	{
		float* data = dst.ptr<float>();
		const int length = dst.rows * dst.cols * channel;

		if(channel > 3)
		{
			#pragma omp parallel for
			for(int i = 0; i < length; i += channel)
			{
				data[i+0] = cvRound(data[i+0] * level) / level;
				data[i+1] = cvRound(data[i+1] * level) / level;
				data[i+2] = cvRound(data[i+2] * level) / level;
			}
		}
		else
		{
			#pragma omp parallel for
			for(int i = 0; i < length; ++i)
				data[i] = cvRound(data[i] * level) / level;
		}
	}
	else
		assert(false);
}

cv::Mat Effect::grayscale(const cv::Mat& image)
{
	Mat gray;
	switch(image.channels())
	{
#if USE_BGRA_LAYOUT
	case 4: cvtColor(image, gray, CV_BGRA2GRAY); break;
	case 3: cvtColor(image, gray, CV_BGR2GRAY);  break;
#else
	case 4: cvtColor(image, gray, CV_RGBA2GRAY); break;
	case 3: cvtColor(image, gray, CV_RGB2GRAY);  break;
#endif
	case 1: image.copyTo(gray);                  break;
	default:
		assert(false);
		break;
	}

	return gray;
}

void Effect::gaussianBlur(cv::Mat& dst, const cv::Mat& src, float radius)
{
	assert(radius >= 0);
	int r = cvRound(radius);
	
	int width = (r << 1) + 1;
	double std_dev = radius * 3;  // 3-sigma rule https://en.wikipedia.org/wiki/68–95–99.7_rule

	cv::GaussianBlur(src, dst, Size(width, width), std_dev, std_dev, cv::BorderTypes::BORDER_CONSTANT);
}

void Effect::gaussianBlurSelective(cv::Mat& dst, const cv::Mat& src, const cv::Mat& mask, float radius, float tolerance)
{
	assert(dst.data != src.data);
	int R = cvRound(radius);
	if(R <= 1)
	{
		src.copyTo(dst);
		return;
	}
	
	int width = (R << 1) + 1;
#if 0
	std::vector<cv::Mat> channels;
	cv::split(src, channels);

	const int N = src.channels() < 4 ? src.channels() : 3;
	#pragma omp parallel for
//	for(cv::Mat& channel: src_channels)  // It seems that OpenMP doesn't support range based for in C++11.
	// error C3016: 'i' : index variable in OpenMP 'for' statement must have signed integral type
	// The Microsoft C/C++ Compiler 12.0 integrated with Visual Studio 2013 still only support OpenMP 2.5 and doesn't allow unsigned int for the loop counter.
	// GCC support OpenMP 3.0 since its version 4.4 and allows unsigned int for the loop counter.
	for(int i = 0; i < N; ++i)
	{
		cv::Mat& channel = channels[i];
		cv::Mat tmp;  // Bilateral filter does not work inplace.
		cv::bilateralFilter(channel, tmp, width, width*2.0, width/2.0);
		channel = tmp;
	}

	cv::merge(channels.data(), channels.size(), dst);
#else
	Mat _src;
	src.convertTo(_src, CV_32F);
//	dst.create(src.rows, src.cols, CV_32F);
	_src.copyTo(dst);
	
	assert(mask.type() == CV_8UC1);
	const uint8_t* const mask_data = mask.ptr<uint8_t>();
//	Mat _mask = normalize(mask);
//	const float* const mask_data = mask.ptr<float>();

	// initialize Gaussian kernel
	Mat kernel(width, width, CV_32FC1);
//	kernel.at<float>(R, R) = 1.0F;
	for(int r = 0; r <= R; ++r)
	for(int c = 0; c <= R; ++c)
	{
		float v = std::exp(-0.5F * (r*r + c*c) / radius);
		kernel.at<float>(R - r, R - c) = v;
		kernel.at<float>(R - r, R + c) = v;
		kernel.at<float>(R + r, R - c) = v;
		kernel.at<float>(R + r, R + c) = v;
	}

	const int channel = src.channels();
	assert(channel >= 3);  // currently only support color image
	const int length = src.rows * src.cols;

	assert(_src.depth() == CV_32F && dst.depth() == CV_32F);
	const float* const _src_data = _src.ptr<float>();
	float* const dst_data = dst.ptr<float>();

	#pragma omp parallel for
	for(int k = 0; k < length; ++k)
	{
		if(mask_data[k] == 0)
			continue;

		int r = k / src.cols, c = k % src.cols;
		int offset = k * channel;
		const float* center = _src_data + offset;
		
		Vec3f accumulated(0, 0, 0), count(0, 0, 0);
		for(int y = -R; y <= R; ++y)
		{
			int j = r + y;
			if(j < 0 || j >= src.rows)
				continue;

			for(int x = -R; x <= R; ++x)
			{
				int i = c + x;
				if(i < 0 || i >= src.cols)
					continue;
				
				float weight = kernel.at<float>(R + y, R + x);
				const float* around = _src_data + (j * src.cols + i) * channel;
#if 1
				// individual channel
				for(int m = 0; m < 3; ++m)
				{
					float diff = std::abs(center[m] - around[m]);
					if(diff > tolerance)
						continue;
					
					accumulated[m] += weight * around[m];
					count[m] += weight;
				}
#else
				// RGB weighted measure method
				float diff = (std::abs(center[0] - around[0]) +
					std::abs(center[1] - around[1]) + std::abs(center[2] - around[2]))/3;
				if(diff > tolerance)
					continue;

				for(int m = 0; m < 3; ++m)
				{
					accumulated[m] += weight * around[m];
					count[m] += weight;
				}
#endif
			}
		}

		// weight_sum can not be ZERO, since center point is counted.
		dst_data[offset + 0] = accumulated[0] / count[0];
		dst_data[offset + 1] = accumulated[1] / count[1];
		dst_data[offset + 2] = accumulated[2] / count[2];
	}

	if(src.depth() == CV_8U)
		dst.convertTo(dst, CV_8U);
#endif
}

float Effect::mapColorBalance(float value, float lightness, float shadows, float midtones, float highlights)
{
	/* Apply masks to the corrections for shadows, midtones and highlights so that each correction affects only one range.
	 * Those masks look like this:
	 *     ‾\___
	 *     _/‾\_
	 *     ___/‾
	 * with ramps of width a at x = b and x = 1 - b.
	 *
	 * The sum of these masks equals 1 for x in 0..1, so applying the same correction in the shadows and in the midtones 
	 * is equivalent to applying this correction on a virtual shadows_and_midtones range.
	 */
	const float a = 0.25F, b = 1.00F/3, scale = 0.70F;
	
	constexpr auto clamp_01 = [](float x) -> float { return clamp<float>(x, 0, 1); };

	shadows = shadows * clamp_01((lightness - b) / -a + 0.5F) * scale;
	midtones = midtones * clamp_01((lightness - b) /  a + 0.5F) *
			clamp_01((lightness + b - 1) / -a + 0.5F) * scale;
	highlights = highlights * clamp_01((lightness + b - 1) / a + 0.5F) * scale;
	
	value += shadows;
	value += midtones;
	value += highlights;
	value = clamp_01(value);
	
	return value;
}

void Effect::adjustColorBalance(float* const dst, const float* const src, int width, int height, const cv::Vec3f config[3], bool preserve_luminosity)
{
	assert(src != nullptr && dst != nullptr);
	constexpr int SHADOWS    = static_cast<int>(RangeMode::SHADOWS);
	constexpr int MIDTONES   = static_cast<int>(RangeMode::MIDTONES);
	constexpr int HIGHLIGHTS = static_cast<int>(RangeMode::HIGHLIGHTS);
	
	for(int i = 0, length = height * width * 4; i < length; i += 4)
	{
		const cv::Vec3f& rgb = src[i];
		cv::Vec3f hsl = rgb2hsl(rgb);

		float& lightness = hsl[2];
		cv::Vec3f rgb2 = dst[i];
		for(int k = 0; k < 3; ++k)
			rgb2[k] = mapColorBalance(rgb[k], lightness, config[SHADOWS][k], config[MIDTONES][k], config[HIGHLIGHTS][k]);

		if(preserve_luminosity)
		{
			cv::Vec3f hsl2 = rgb2hsl(rgb2);
			hsl2[2] = hsl[2];  // preserve brightness info
			rgb2 = hsl2rgb(hsl2);
		}

		dst[i+3] = src[i+3];  // keep alpha

		for(int k = 0; k < 3; ++k)
			assert(0 <= dst[k] && dst[k] <= 1);
	}
}

void Effect::adjustGamma(cv::Mat& dst, const cv::Mat& src, float gamma)
{
	assert(gamma > 0);
	gamma = 1/gamma;

	dst.create(src.rows, src.cols, src.type());
	int depth = src.depth(), channel = src.channels();
	if(depth == CV_8U)
	{
		// build a lookup table mapping the pixel values [0, 255] to their adjusted gamma values
		uint8_t table[256];
		for(int i = 0; i < 256; ++i)
			table[i] = cvRound(std::pow(i/255.0, gamma) * 255.0);

		if(channel == 4)
		{
			const cv::Vec4b* src_data = reinterpret_cast<const cv::Vec4b*>(src.data);
			cv::Vec4b* const dst_data = reinterpret_cast<cv::Vec4b* const>(dst.data);
			const int length = src.rows * src.cols;

			#pragma omp parallel for
			for(int i = 0; i < length; ++i)
			{
				const cv::Vec4b& s = src_data[i];
				cv::Vec4b& d = dst_data[i];
				for(int i = 0; i < 3; ++i)  // loop unrolling
					d[i] = table[s[i]];
			}
		}
		else
		{
			const uint8_t* src_data = src.data;
			uint8_t* const dst_data = dst.data;
			const int length = src.rows * src.cols * channel;

			#pragma omp parallel for
			for(int i = 0; i < length; ++i)
				dst_data[i] = table[src_data[i]];
		}
	}
	else if(depth == CV_32F && channel == 4)
	{
		const float* src_data = reinterpret_cast<const float*>(src.data);
		float* const dst_data = reinterpret_cast<float* const>(dst.data);
		const int channels = src.channels();
		const int length = src.rows * src.cols * channels;

		if(channels == 4)  // has alpha
		{
			#pragma omp parallel for
			for(int i = 0; i < length; i += 4)
			{
				for(int k = 0; i < 3; ++k)
					dst_data[i+k] = std::pow(src_data[i+k], gamma);
				dst_data[i+3] = src_data[i+3];  // keep alpha untouched
			}
		}
		else
			#pragma omp parallel for
			for(int i = 0; i < length; ++i)
				dst_data[i] = std::pow(src_data[i], gamma);
	}
	else
		assert(false);  // unimplemented branch goes here.
}

void Effect::adjustGamma(cv::Mat& dst, const cv::Mat& src, const cv::Vec3f& gamma)
{
	assert(src.channels() >= 3);

	std::vector<cv::Mat> channels;
	cv::split(src, channels);

	for(int i = 0; i < 3; ++i)  // skip alpha channel if it has.
		adjustGamma(channels[i], channels[i], gamma[i]);

	cv::merge(channels, dst);
}
} /* namespace venus */
