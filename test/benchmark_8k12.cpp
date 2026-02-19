// Copyright (c) Team CharLS.
// SPDX-License-Identifier: BSD-3-Clause

#include <charls/charls.hpp>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <numeric>
#include <vector>

using std::byte;
using std::cout;
using std::vector;
using std::chrono::duration;
using std::chrono::steady_clock;

namespace {

constexpr uint32_t width{7680};
constexpr uint32_t height{4320};
constexpr int32_t bits_per_sample{12};
constexpr int32_t component_count{1};
constexpr int32_t max_value{(1 << bits_per_sample) - 1};
constexpr int default_loop_count{10};

/// <summary>
/// Generates synthetic 12-bit mono image data with a gradient + deterministic noise pattern
/// that produces realistic compression behavior (not trivially compressible, not random).
/// </summary>
vector<uint16_t> generate_test_image()
{
    vector<uint16_t> image(static_cast<size_t>(width) * height);

    // Simple LCG for deterministic pseudo-random noise
    uint32_t seed{42};
    auto next_random = [&seed]() -> int32_t {
        seed = seed * 1103515245U + 12345U;
        return static_cast<int32_t>((seed >> 16) & 0x7FFF);
    };

    for (uint32_t y{}; y < height; ++y)
    {
        for (uint32_t x{}; x < width; ++x)
        {
            // Smooth horizontal + vertical gradient as base signal
            const int32_t gradient{static_cast<int32_t>((static_cast<uint64_t>(x) * max_value / width +
                                                         static_cast<uint64_t>(y) * max_value / height) /
                                                        2)};

            // Add low-amplitude noise (similar to sensor noise in raw camera data)
            // Range: [-32, +31] => ~1.5% of 12-bit range
            const int32_t noise{(next_random() % 64) - 32};

            const int32_t value{std::clamp(gradient + noise, 0, max_value)};
            image[static_cast<size_t>(y) * width + x] = static_cast<uint16_t>(value);
        }
    }

    return image;
}

void run_benchmark(const int loop_count)
{
    cout << "=== CharLS 8K 12-Bit Mono Benchmark ===\n";
    cout << "Image: " << width << "x" << height << " " << bits_per_sample << "-bit mono\n";
    cout << "Pixel count: " << static_cast<size_t>(width) * height << "\n";
    const size_t raw_size_bytes{static_cast<size_t>(width) * height * sizeof(uint16_t)};
    cout << "Raw size: " << raw_size_bytes / (1024 * 1024) << " MiB\n";
    cout << "Loop count: " << loop_count << "\n\n";

    // Generate test data
    cout << "Generating synthetic 12-bit test image...\n";
    const auto image{generate_test_image()};

    const charls::frame_info info{width, height, bits_per_sample, component_count};

    // Pre-allocate encoder destination
    charls::jpegls_encoder size_encoder;
    size_encoder.frame_info(info);
    vector<byte> encoded(size_encoder.estimated_destination_size());

    // --- Encode benchmark ---
    cout << "Running encode benchmark (" << loop_count << " iterations)...\n";

    size_t encoded_size{};
    vector<double> encode_times;
    encode_times.reserve(loop_count);

    for (int i{}; i < loop_count; ++i)
    {
        charls::jpegls_encoder encoder;
        encoder.frame_info(info);
        encoder.destination(encoded);

        const auto t0{steady_clock::now()};
        encoded_size = encoder.encode(image.data(), image.size() * sizeof(uint16_t));
        const auto t1{steady_clock::now()};

        encode_times.push_back(duration<double, std::milli>(t1 - t0).count());
    }

    std::sort(encode_times.begin(), encode_times.end());
    const double encode_median{encode_times[encode_times.size() / 2]};
    const double encode_mean{std::accumulate(encode_times.begin(), encode_times.end(), 0.0) /
                             static_cast<double>(encode_times.size())};
    const double encode_min{encode_times.front()};
    const double raw_mb{static_cast<double>(raw_size_bytes) / (1024.0 * 1024.0)};

    cout << "  Encoded size: " << encoded_size << " bytes ("
         << static_cast<double>(encoded_size) * 100.0 / static_cast<double>(raw_size_bytes) << "%)\n";
    cout << "  Compression ratio: " << static_cast<double>(raw_size_bytes) / static_cast<double>(encoded_size) << ":1\n";
    cout << "  Encode min:    " << encode_min << " ms (" << raw_mb / (encode_min / 1000.0) << " MB/s)\n";
    cout << "  Encode median: " << encode_median << " ms (" << raw_mb / (encode_median / 1000.0) << " MB/s)\n";
    cout << "  Encode mean:   " << encode_mean << " ms (" << raw_mb / (encode_mean / 1000.0) << " MB/s)\n\n";

    // --- Decode benchmark ---
    cout << "Running decode benchmark (" << loop_count << " iterations)...\n";

    vector<uint16_t> decoded(static_cast<size_t>(width) * height);
    vector<double> decode_times;
    decode_times.reserve(loop_count);

    for (int i{}; i < loop_count; ++i)
    {
        charls::jpegls_decoder decoder{encoded.data(), encoded_size, true};

        const auto t0{steady_clock::now()};
        decoder.decode(decoded.data(), decoded.size() * sizeof(uint16_t));
        const auto t1{steady_clock::now()};

        decode_times.push_back(duration<double, std::milli>(t1 - t0).count());
    }

    std::sort(decode_times.begin(), decode_times.end());
    const double decode_median{decode_times[decode_times.size() / 2]};
    const double decode_mean{std::accumulate(decode_times.begin(), decode_times.end(), 0.0) /
                             static_cast<double>(decode_times.size())};
    const double decode_min{decode_times.front()};

    cout << "  Decode min:    " << decode_min << " ms (" << raw_mb / (decode_min / 1000.0) << " MB/s)\n";
    cout << "  Decode median: " << decode_median << " ms (" << raw_mb / (decode_median / 1000.0) << " MB/s)\n";
    cout << "  Decode mean:   " << decode_mean << " ms (" << raw_mb / (decode_mean / 1000.0) << " MB/s)\n\n";

    // --- Round-trip verification ---
    cout << "Verifying round-trip correctness... ";
    if (image == decoded)
    {
        cout << "PASS\n";
    }
    else
    {
        cout << "FAIL\n";
        size_t mismatch_count{};
        for (size_t i{}; i < image.size(); ++i)
        {
            if (image[i] != decoded[i])
            {
                if (mismatch_count < 5)
                {
                    cout << "  Mismatch at index " << i << ": expected " << image[i] << ", got " << decoded[i] << "\n";
                }
                ++mismatch_count;
            }
        }
        cout << "  Total mismatches: " << mismatch_count << " / " << image.size() << "\n";
        std::exit(1);
    }

    // --- Summary line (easy to parse) ---
    cout << "\nSUMMARY: encode_median_ms=" << encode_median << " decode_median_ms=" << decode_median
         << " encode_MB_s=" << raw_mb / (encode_median / 1000.0) << " decode_MB_s=" << raw_mb / (decode_median / 1000.0)
         << " ratio=" << static_cast<double>(raw_size_bytes) / static_cast<double>(encoded_size) << "\n";
}

} // namespace

int main(const int argc, const char* const argv[])
{
    int loop_count{default_loop_count};

    if (argc > 1)
    {
        loop_count = std::atoi(argv[1]);
        if (loop_count < 1)
        {
            cout << "Usage: " << argv[0] << " [loop_count]\n";
            return 1;
        }
    }

    try
    {
        run_benchmark(loop_count);
    }
    catch (const charls::jpegls_error& e)
    {
        cout << "CharLS error: " << e.what() << "\n";
        return 1;
    }
    catch (const std::exception& e)
    {
        cout << "Error: " << e.what() << "\n";
        return 1;
    }

    return 0;
}
