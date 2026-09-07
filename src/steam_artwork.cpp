/** @file src/steam_artwork.cpp */
#include "steam_artwork.h"
#include "provider_scan_protocol.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <cstring>
#include <fstream>
#include <iterator>
#include <limits>
#include <optional>
#include <sstream>
#include <thread>

#ifdef SUNSHINE_STEAM_ARTWORK_NETWORK
  #include <curl/curl.h>
  #include "httpcommon.h"
#endif

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/error.h>
#include <libavutil/imgutils.h>
#include <libavutil/pixfmt.h>
#include <libswscale/swscale.h>
}

#ifdef VIBESHINE_STEAM_ARTWORK_IMAGE_LIBS
extern "C" {
#include <jpeglib.h>
#include <png.h>
#include <webp/decode.h>
}
#include <setjmp.h>
#endif

#ifdef _WIN32
  #include <windows.h>
#endif

namespace fs = std::filesystem;

namespace {
  constexpr std::uint8_t png_signature[] = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1a, '\n'};
  constexpr std::size_t max_remote_bytes = 16U * 1024U * 1024U;
  constexpr int full_portrait_width = 600;
  constexpr int full_portrait_height = 900;

  fs::path sidecar_path(const fs::path &output) {
    return output.string() + ".meta";
  }

  bool regular(const fs::path &path) {
    std::error_code ec;
    return fs::is_regular_file(path, ec) && !ec;
  }

  bool valid_png(const fs::path &path) {
    std::ifstream input(path, std::ios::binary);
    std::array<std::uint8_t, sizeof(png_signature)> bytes {};
    if (!input.read(reinterpret_cast<char *>(bytes.data()), static_cast<std::streamsize>(bytes.size()))) {
      return false;
    }
    return std::equal(bytes.begin(), bytes.end(), std::begin(png_signature));
  }

  struct image_dimensions_t {
    int width = 0;
    int height = 0;
  };

  std::optional<image_dimensions_t> png_dimensions(const std::vector<std::uint8_t> &bytes) {
    if (bytes.size() < 24 || !std::equal(std::begin(png_signature), std::end(png_signature), bytes.begin()) ||
        std::memcmp(bytes.data() + 12, "IHDR", 4) != 0) {
      return std::nullopt;
    }
    const auto read_u32 = [&](std::size_t offset) {
      return (static_cast<std::uint32_t>(bytes[offset]) << 24) |
             (static_cast<std::uint32_t>(bytes[offset + 1]) << 16) |
             (static_cast<std::uint32_t>(bytes[offset + 2]) << 8) |
             static_cast<std::uint32_t>(bytes[offset + 3]);
    };
    const auto width = read_u32(16);
    const auto height = read_u32(20);
    if (width == 0 || height == 0 || width > static_cast<std::uint32_t>(std::numeric_limits<int>::max()) ||
        height > static_cast<std::uint32_t>(std::numeric_limits<int>::max())) {
      return std::nullopt;
    }
    return image_dimensions_t {static_cast<int>(width), static_cast<int>(height)};
  }

  std::uint32_t png_crc32(const std::uint8_t *bytes, std::size_t size) {
    std::uint32_t crc = 0xffffffffU;
    for (std::size_t index = 0; index < size; ++index) {
      crc ^= bytes[index];
      for (int bit = 0; bit < 8; ++bit) crc = (crc >> 1) ^ (0xedb88320U & -(crc & 1U));
    }
    return ~crc;
  }

  bool valid_png_bytes(const std::vector<std::uint8_t> &bytes) {
    if (bytes.size() < sizeof(png_signature) ||
        !std::equal(std::begin(png_signature), std::end(png_signature), bytes.begin())) return false;
    std::size_t offset = sizeof(png_signature);
    while (offset + 12 <= bytes.size()) {
      const auto length = (static_cast<std::size_t>(bytes[offset]) << 24) |
                          (static_cast<std::size_t>(bytes[offset + 1]) << 16) |
                          (static_cast<std::size_t>(bytes[offset + 2]) << 8) |
                          static_cast<std::size_t>(bytes[offset + 3]);
      if (length > bytes.size() - offset - 12) return false;
      const auto crc = (static_cast<std::uint32_t>(bytes[offset + 8 + length]) << 24) |
                       (static_cast<std::uint32_t>(bytes[offset + 9 + length]) << 16) |
                       (static_cast<std::uint32_t>(bytes[offset + 10 + length]) << 8) |
                       static_cast<std::uint32_t>(bytes[offset + 11 + length]);
      if (png_crc32(bytes.data() + offset + 4, length + 4) != crc) return false;
      const bool end = std::memcmp(bytes.data() + offset + 4, "IEND", 4) == 0;
      offset += length + 12;
      if (end) return length == 0 && offset == bytes.size();
    }
    return false;
  }

  std::optional<platf::steam::artwork::source_fingerprint_t> fingerprint(const fs::path &source) {
    std::error_code ec;
    if (!fs::is_regular_file(source, ec) || ec) return std::nullopt;
    auto canonical = fs::weakly_canonical(source, ec);
    if (ec) canonical = fs::absolute(source, ec);
    if (ec) return std::nullopt;
    const auto size = fs::file_size(canonical, ec);
    if (ec) return std::nullopt;
    const auto mtime = fs::last_write_time(canonical, ec).time_since_epoch().count();
    if (ec) return std::nullopt;
    return platf::steam::artwork::source_fingerprint_t {canonical.generic_string(), size, mtime};
  }

  bool read_meta(const fs::path &path, platf::steam::artwork::source_fingerprint_t &out) {
    std::ifstream input(path);
    if (!input) return false;
    std::string line;
    while (std::getline(input, line)) {
      const auto split = line.find('=');
      if (split == std::string::npos) continue;
      const auto key = line.substr(0, split);
      const auto value = line.substr(split + 1);
      try {
        if (key == "path") out.path = value;
        else if (key == "size") out.size = static_cast<std::uintmax_t>(std::stoull(value));
        else if (key == "mtime") out.mtime = static_cast<std::intmax_t>(std::stoll(value));
      } catch (...) {
        return false;
      }
    }
    return !out.path.empty();
  }

  std::string unique_suffix() {
    const auto now = std::chrono::steady_clock::now().time_since_epoch().count();
    const auto thread_id = std::hash<std::thread::id> {}(std::this_thread::get_id());
    return std::to_string(now) + "." + std::to_string(thread_id);
  }

  bool replace_atomically(const fs::path &temporary, const fs::path &target) {
#ifdef _WIN32
    return MoveFileExW(temporary.wstring().c_str(), target.wstring().c_str(),
                       MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0;
#else
    std::error_code ec;
    fs::rename(temporary, target, ec);
    return !ec;
#endif
  }

  bool write_meta(const fs::path &path, const platf::steam::artwork::source_fingerprint_t &fp) {
    const auto temporary = path.string() + ".tmp." + unique_suffix();
    {
      std::ofstream output(temporary, std::ios::trunc);
      if (!output) return false;
      output << "version=1\npath=" << fp.path << "\nsize=" << fp.size << "\nmtime=" << fp.mtime << "\n";
      output.flush();
      if (!output) {
        std::error_code ec;
        fs::remove(temporary, ec);
        return false;
      }
    }
    if (!replace_atomically(temporary, path)) {
      std::error_code ec;
      fs::remove(temporary, ec);
      return false;
    }
    return true;
  }

  std::vector<std::uint8_t> read_bytes(const fs::path &path) {
    std::ifstream input(path, std::ios::binary);
    if (!input) return {};
    return std::vector<std::uint8_t> {std::istreambuf_iterator<char>(input), {}};
  }

  std::vector<std::uint8_t> read_prefix(const fs::path &path, std::size_t limit = 1024U * 1024U) {
    std::ifstream input(path, std::ios::binary);
    if (!input) return {};
    std::vector<std::uint8_t> bytes(limit);
    input.read(reinterpret_cast<char *>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    bytes.resize(static_cast<std::size_t>(input.gcount()));
    return bytes;
  }

  std::optional<image_dimensions_t> probe_dimensions(const fs::path &path) {
    const auto bytes = read_prefix(path);
    if (bytes.size() >= 24 && std::equal(std::begin(png_signature), std::end(png_signature), bytes.begin()) &&
        std::memcmp(bytes.data() + 12, "IHDR", 4) == 0) {
      const auto read_u32 = [&](std::size_t offset) {
        return (static_cast<std::uint32_t>(bytes[offset]) << 24) |
               (static_cast<std::uint32_t>(bytes[offset + 1]) << 16) |
               (static_cast<std::uint32_t>(bytes[offset + 2]) << 8) |
               static_cast<std::uint32_t>(bytes[offset + 3]);
      };
      const auto width = read_u32(16);
      const auto height = read_u32(20);
      if (width > 0 && height > 0 && width <= static_cast<std::uint32_t>(std::numeric_limits<int>::max()) &&
          height <= static_cast<std::uint32_t>(std::numeric_limits<int>::max())) {
        return image_dimensions_t {static_cast<int>(width), static_cast<int>(height)};
      }
      return std::nullopt;
    }
    if (bytes.size() >= 16 && std::memcmp(bytes.data(), "RIFF", 4) == 0 && std::memcmp(bytes.data() + 8, "WEBP", 4) == 0) {
      std::size_t chunk = 12;
      while (chunk + 8 <= bytes.size()) {
        const auto size = static_cast<std::size_t>(bytes[chunk + 4]) |
                          (static_cast<std::size_t>(bytes[chunk + 5]) << 8) |
                          (static_cast<std::size_t>(bytes[chunk + 6]) << 16) |
                          (static_cast<std::size_t>(bytes[chunk + 7]) << 24);
        if (chunk + 8 + size > bytes.size()) break;
        if (std::memcmp(bytes.data() + chunk, "VP8X", 4) == 0 && size >= 10) {
          const auto width = 1U + static_cast<std::uint32_t>(bytes[chunk + 8 + 4]) +
                             (static_cast<std::uint32_t>(bytes[chunk + 8 + 5]) << 8) +
                             (static_cast<std::uint32_t>(bytes[chunk + 8 + 6]) << 16);
          const auto height = 1U + static_cast<std::uint32_t>(bytes[chunk + 8 + 7]) +
                              (static_cast<std::uint32_t>(bytes[chunk + 8 + 8]) << 8) +
                              (static_cast<std::uint32_t>(bytes[chunk + 8 + 9]) << 16);
          return image_dimensions_t {static_cast<int>(width), static_cast<int>(height)};
        }
        chunk += 8 + size + (size & 1U);
      }
    }
    if (bytes.size() >= 2 && bytes[0] == 0xff && bytes[1] == 0xd8) {
      std::size_t pos = 2;
      while (pos + 4 <= bytes.size()) {
        if (bytes[pos++] != 0xff) continue;
        while (pos < bytes.size() && bytes[pos] == 0xff) ++pos;
        if (pos >= bytes.size()) break;
        const auto marker = bytes[pos++];
        if (marker == 0xd8 || marker == 0xd9) continue;
        if (marker == 0xda) break;
        const auto length = (static_cast<std::size_t>(bytes[pos]) << 8) | bytes[pos + 1];
        if (length < 2 || pos + length > bytes.size()) break;
        const bool sof = (marker >= 0xc0 && marker <= 0xc3) || (marker >= 0xc5 && marker <= 0xc7) ||
                         (marker >= 0xc9 && marker <= 0xcb) || (marker >= 0xcd && marker <= 0xcf);
        if (sof && length >= 7) {
          const auto height = (static_cast<int>(bytes[pos + 3]) << 8) | bytes[pos + 4];
          const auto width = (static_cast<int>(bytes[pos + 5]) << 8) | bytes[pos + 6];
          if (width > 0 && height > 0) return image_dimensions_t {width, height};
        }
        pos += length;
      }
    }
    return std::nullopt;
  }

  AVCodecID codec_id_for(const fs::path &path, const std::vector<std::uint8_t> &bytes) {
    // Steam occasionally leaves a stale extension while replacing cache
    // files. Prefer the actual signature so a test/provider fixture and the
    // downloaded cache are validated consistently after an atomic rename.
    if (bytes.size() >= 8 && std::equal(bytes.begin(), bytes.begin() + 8, png_signature)) return AV_CODEC_ID_PNG;
    if (bytes.size() >= 2 && bytes[0] == 0xff && bytes[1] == 0xd8) return AV_CODEC_ID_MJPEG;
    if (bytes.size() >= 12 && std::memcmp(bytes.data(), "RIFF", 4) == 0 && std::memcmp(bytes.data() + 8, "WEBP", 4) == 0) {
      return AV_CODEC_ID_WEBP;
    }
    std::string ext = path.extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    if (ext == ".jpg" || ext == ".jpeg") return AV_CODEC_ID_MJPEG;
    if (ext == ".png") return AV_CODEC_ID_PNG;
    if (ext == ".webp") return AV_CODEC_ID_WEBP;
    return AV_CODEC_ID_NONE;
  }

#ifdef VIBESHINE_STEAM_ARTWORK_IMAGE_LIBS
  struct rgba_image_t {
    int width = 0;
    int height = 0;
    std::vector<std::uint8_t> pixels;
  };

  struct jpeg_error_t {
    jpeg_error_mgr base;
    jmp_buf jump;
  };

  void jpeg_error_exit(j_common_ptr info) {
    auto *error = reinterpret_cast<jpeg_error_t *>(info->err);
    longjmp(error->jump, 1);
  }

  std::optional<rgba_image_t> decode_jpeg(const std::vector<std::uint8_t> &bytes) {
    jpeg_decompress_struct decoder {};
    jpeg_error_t error {};
    decoder.err = jpeg_std_error(&error.base);
    error.base.error_exit = jpeg_error_exit;
    if (setjmp(error.jump) != 0) {
      jpeg_destroy_decompress(&decoder);
      return std::nullopt;
    }
    jpeg_create_decompress(&decoder);
    jpeg_mem_src(&decoder, const_cast<unsigned char *>(bytes.data()), bytes.size());
    jpeg_read_header(&decoder, TRUE);
    decoder.out_color_space = JCS_RGB;
    jpeg_start_decompress(&decoder);
    rgba_image_t image {static_cast<int>(decoder.output_width), static_cast<int>(decoder.output_height),
                        std::vector<std::uint8_t>(decoder.output_width * decoder.output_height * 4)};
    std::vector<std::uint8_t> row(decoder.output_width * 3);
    while (decoder.output_scanline < decoder.output_height) {
      unsigned char *scanline = row.data();
      jpeg_read_scanlines(&decoder, &scanline, 1);
      const auto y = decoder.output_scanline - 1;
      for (unsigned int x = 0; x < decoder.output_width; ++x) {
        const auto source = x * 3;
        const auto target = (y * decoder.output_width + x) * 4;
        image.pixels[target] = row[source];
        image.pixels[target + 1] = row[source + 1];
        image.pixels[target + 2] = row[source + 2];
        image.pixels[target + 3] = 255;
      }
    }
    jpeg_finish_decompress(&decoder);
    jpeg_destroy_decompress(&decoder);
    return image;
  }

  std::optional<rgba_image_t> decode_webp(const std::vector<std::uint8_t> &bytes) {
    int width = 0;
    int height = 0;
    auto *decoded = WebPDecodeRGBA(bytes.data(), bytes.size(), &width, &height);
    if (!decoded || width <= 0 || height <= 0) {
      if (decoded) WebPFree(decoded);
      return std::nullopt;
    }
    rgba_image_t image {width, height, std::vector<std::uint8_t>(decoded, decoded + width * height * 4)};
    WebPFree(decoded);
    return image;
  }

  std::optional<std::vector<std::uint8_t>> encode_png(const rgba_image_t &image) {
    png_structp writer = png_create_write_struct(PNG_LIBPNG_VER_STRING, nullptr, nullptr, nullptr);
    png_infop info = writer ? png_create_info_struct(writer) : nullptr;
    if (!writer || !info) {
      if (writer) png_destroy_write_struct(&writer, &info);
      return std::nullopt;
    }
    std::vector<std::uint8_t> output;
    if (setjmp(png_jmpbuf(writer)) != 0) {
      png_destroy_write_struct(&writer, &info);
      return std::nullopt;
    }
    png_set_write_fn(writer, &output,
      [](png_structp png, png_bytep bytes, png_size_t size) {
        auto &target = *static_cast<std::vector<std::uint8_t> *>(png_get_io_ptr(png));
        target.insert(target.end(), bytes, bytes + size);
      }, nullptr);
    png_set_IHDR(writer, info, image.width, image.height, 8, PNG_COLOR_TYPE_RGBA,
                 PNG_INTERLACE_NONE, PNG_COMPRESSION_TYPE_DEFAULT, PNG_FILTER_TYPE_DEFAULT);
    png_write_info(writer, info);
    for (int y = 0; y < image.height; ++y) {
      png_write_row(writer, const_cast<png_bytep>(image.pixels.data() + y * image.width * 4));
    }
    png_write_end(writer, info);
    png_destroy_write_struct(&writer, &info);
    return output;
  }

  std::optional<std::vector<std::uint8_t>> convert_with_image_libs(const fs::path &source,
                                                                     const std::vector<std::uint8_t> &bytes) {
    const auto id = codec_id_for(source, bytes);
    if (id == AV_CODEC_ID_PNG && bytes.size() >= sizeof(png_signature) &&
        std::equal(std::begin(png_signature), std::end(png_signature), bytes.begin())) return bytes;
    std::optional<rgba_image_t> image;
    if (id == AV_CODEC_ID_MJPEG) image = decode_jpeg(bytes);
    else if (id == AV_CODEC_ID_WEBP) image = decode_webp(bytes);
    return image ? encode_png(*image) : std::nullopt;
  }
#endif

  std::optional<std::vector<std::uint8_t>> convert_to_png(const fs::path &source) {
    const auto bytes = read_bytes(source);
    if (bytes.empty()) return std::nullopt;
#ifdef VIBESHINE_STEAM_ARTWORK_IMAGE_LIBS
    if (const auto converted = convert_with_image_libs(source, bytes)) return converted;
#endif
    const auto id = codec_id_for(source, bytes);
    if (id == AV_CODEC_ID_NONE) return std::nullopt;
    const auto *decoder = avcodec_find_decoder(id);
    if (!decoder) return std::nullopt;
    AVCodecContext *decoder_context = avcodec_alloc_context3(decoder);
    AVPacket *input_packet = av_packet_alloc();
    AVFrame *decoded = av_frame_alloc();
    if (!decoder_context || !input_packet || !decoded) {
      avcodec_free_context(&decoder_context);
      av_packet_free(&input_packet);
      av_frame_free(&decoded);
      return std::nullopt;
    }
    auto fail_decode = [&]() -> std::optional<std::vector<std::uint8_t>> {
      avcodec_free_context(&decoder_context);
      av_packet_free(&input_packet);
      av_frame_free(&decoded);
      return std::nullopt;
    };
    const auto decoder_open = avcodec_open2(decoder_context, decoder, nullptr);
    const auto packet_alloc = av_new_packet(input_packet, static_cast<int>(bytes.size()));
    if (decoder_open < 0 || packet_alloc < 0) {
      return fail_decode();
    }
    std::memcpy(input_packet->data, bytes.data(), bytes.size());
    input_packet->pts = 0;
    const auto packet_result = avcodec_send_packet(decoder_context, input_packet);
    const auto frame_result = avcodec_receive_frame(decoder_context, decoded);
    if (packet_result < 0 || frame_result < 0 ||
        decoded->width <= 0 || decoded->height <= 0) {
      return fail_decode();
    }

    const auto *encoder = avcodec_find_encoder(AV_CODEC_ID_PNG);
    if (!encoder) return fail_decode();
    AVCodecContext *encoder_context = avcodec_alloc_context3(encoder);
    AVFrame *encoded_frame = av_frame_alloc();
    AVPacket *output_packet = av_packet_alloc();
    SwsContext *sws = nullptr;
    if (!encoder_context || !encoded_frame || !output_packet) {
      avcodec_free_context(&encoder_context);
      av_frame_free(&encoded_frame);
      av_packet_free(&output_packet);
      return fail_decode();
    }
    auto supports = [&](AVPixelFormat format) {
      const void *supported_configs = nullptr;
      int supported_config_count = 0;
      if (avcodec_get_supported_config(
            encoder_context,
            encoder,
            AV_CODEC_CONFIG_PIX_FORMAT,
            0,
            &supported_configs,
            &supported_config_count
          ) < 0 || !supported_configs) {
        return false;
      }
      const auto *formats = static_cast<const AVPixelFormat *>(supported_configs);
      for (int i = 0; i < supported_config_count; ++i) {
        if (formats[i] == format) return true;
      }
      return false;
    };
    const auto output_format = supports(AV_PIX_FMT_RGBA) ? AV_PIX_FMT_RGBA : AV_PIX_FMT_RGB24;
    encoder_context->width = decoded->width;
    encoder_context->height = decoded->height;
    encoder_context->pix_fmt = output_format;
    encoder_context->time_base = AVRational {1, 25};
    encoded_frame->format = output_format;
    encoded_frame->width = decoded->width;
    encoded_frame->height = decoded->height;
    const auto encoder_open = avcodec_open2(encoder_context, encoder, nullptr);
    const auto frame_buffer = av_frame_get_buffer(encoded_frame, 1);
    if (encoder_open < 0 || frame_buffer < 0) {
      avcodec_free_context(&encoder_context);
      av_frame_free(&encoded_frame);
      av_packet_free(&output_packet);
      return fail_decode();
    }
    sws = sws_getContext(decoded->width, decoded->height, static_cast<AVPixelFormat>(decoded->format),
                         decoded->width, decoded->height, output_format, SWS_BILINEAR, nullptr, nullptr, nullptr);
    const auto scaled = sws ? sws_scale(sws, decoded->data, decoded->linesize, 0, decoded->height,
                                         encoded_frame->data, encoded_frame->linesize) : 0;
    const auto send_frame = (sws && scaled > 0) ? avcodec_send_frame(encoder_context, encoded_frame) : AVERROR(EINVAL);
    if (!sws || scaled <= 0 || send_frame < 0) {
      sws_freeContext(sws);
      avcodec_free_context(&encoder_context);
      av_frame_free(&encoded_frame);
      av_packet_free(&output_packet);
      return fail_decode();
    }
    std::vector<std::uint8_t> output;
    auto receive_packets = [&]() {
      while (true) {
        const int result = avcodec_receive_packet(encoder_context, output_packet);
        if (result == AVERROR(EAGAIN) || result == AVERROR_EOF) return result;
        if (result < 0) return result;
        output.insert(output.end(), output_packet->data, output_packet->data + output_packet->size);
        av_packet_unref(output_packet);
      }
    };
    const auto first_receive = receive_packets();
    if (first_receive == AVERROR(EAGAIN)) {
      // Flush delayed encoders as well. PNG normally emits immediately, but
      // this keeps the conversion contract correct for alternate FFmpeg
      // encoder implementations.
      if (avcodec_send_frame(encoder_context, nullptr) >= 0) receive_packets();
    }
    sws_freeContext(sws);
    avcodec_free_context(&encoder_context);
    av_frame_free(&encoded_frame);
    av_packet_free(&output_packet);
    avcodec_free_context(&decoder_context);
    av_packet_free(&input_packet);
    av_frame_free(&decoded);
    if (output.size() < sizeof(png_signature) || !std::equal(std::begin(png_signature), std::end(png_signature), output.begin())) {
      return std::nullopt;
    }
    return output;
  }

  bool write_output(const fs::path &target, const std::vector<std::uint8_t> &bytes) {
    const auto temporary = target.string() + ".tmp." + unique_suffix();
    {
      std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
      if (!output) return false;
      output.write(reinterpret_cast<const char *>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
      output.flush();
      if (!output) {
        std::error_code ec;
        fs::remove(temporary, ec);
        return false;
      }
    }
    if (!replace_atomically(temporary, target)) {
      std::error_code ec;
      fs::remove(temporary, ec);
      return false;
    }
    return true;
  }

  bool full_portrait(const fs::path &source) {
    const auto dimensions = probe_dimensions(source);
    return dimensions && dimensions->width >= full_portrait_width && dimensions->height >= full_portrait_height;
  }

  bool recent_failure_marker(const fs::path &marker) {
    std::error_code ec;
    if (!regular(marker)) return false;
    const auto modified = fs::last_write_time(marker, ec);
    if (ec) return false;
    using clock = decltype(modified)::clock;
    const auto age = clock::now() - modified;
    return age >= decltype(age)::zero() && age < std::chrono::minutes(10);
  }

  void mark_remote_failure(const fs::path &marker) {
    const std::vector<std::uint8_t> marker_bytes {'1'};
    write_output(marker, marker_bytes);
  }

#ifdef SUNSHINE_STEAM_ARTWORK_NETWORK
  struct curl_buffer_t {
    std::vector<std::uint8_t> bytes;
    bool overflow = false;
  };

  std::size_t curl_write(void *contents, std::size_t size, std::size_t count, void *user) {
    auto *buffer = static_cast<curl_buffer_t *>(user);
    if (count != 0 && size > max_remote_bytes / count) {
      buffer->overflow = true;
      return 0;
    }
    const auto total = size * count;
    if (total > max_remote_bytes - buffer->bytes.size()) {
      buffer->overflow = true;
      return 0;
    }
    const auto *begin = static_cast<const std::uint8_t *>(contents);
    buffer->bytes.insert(buffer->bytes.end(), begin, begin + total);
    return total;
  }

  std::optional<std::vector<std::uint8_t>> fetch_remote(const std::string &url) {
    CURL *curl = curl_easy_init();
    if (!curl) return std::nullopt;
    if (!http::configure_curl_tls(curl)) {
      curl_easy_cleanup(curl);
      return std::nullopt;
    }
    curl_buffer_t buffer;
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 0L);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT_MS, 3000L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, 10000L);
    curl_easy_setopt(curl, CURLOPT_LOW_SPEED_LIMIT, 1024L);
    curl_easy_setopt(curl, CURLOPT_LOW_SPEED_TIME, 5L);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    // Never weaken certificate or hostname verification for artwork.
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 1L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 2L);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "Vibepollo-Steam-Artwork/1.0");
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, curl_write);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &buffer);
    const auto result = curl_easy_perform(curl);
    long status = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
    curl_easy_cleanup(curl);
    if (result != CURLE_OK || buffer.overflow || status < 200 || status >= 300 || buffer.bytes.empty()) {
      return std::nullopt;
    }
    return std::move(buffer.bytes);
  }
#else
  std::optional<std::vector<std::uint8_t>> fetch_remote(const std::string &) {
    // Component tests inject a fetcher; builds without the application HTTP
    // stack remain offline by construction.
    return std::nullopt;
  }
#endif

  fs::path obtain_remote_portrait(std::uint32_t app_id, const fs::path &appdata,
                                  const platf::steam::artwork::remote_fetcher_t &injected_fetcher) {
    if (app_id == 0) return {};
    const auto cache = platf::steam::artwork::remote_cache_path(appdata, app_id);
    const auto failure = cache.string() + ".failed";
    std::error_code ec;
    fs::create_directories(cache.parent_path(), ec);
    // Steam's CDN asset is immutable for the app ID in practice. Treat the
    // validated file as a stable source and fetch only when it is absent or
    // unusable; the 30-second catalog poll therefore never redownloads it.
    if (regular(cache) && full_portrait(cache)) return cache;
    if (recent_failure_marker(failure)) return {};

    const auto fetcher = injected_fetcher ? injected_fetcher : fetch_remote;
    const auto bytes = fetcher(platf::steam::artwork::remote_portrait_url(app_id));
    if (!bytes || bytes->empty() || bytes->size() > max_remote_bytes) {
      mark_remote_failure(failure);
      return {};
    }

    const auto temporary = cache.string() + ".download." + unique_suffix();
    if (!write_output(temporary, *bytes)) {
      mark_remote_failure(failure);
      return {};
    }
    const auto downloaded = read_bytes(temporary);
    const auto downloaded_id = codec_id_for(temporary, downloaded);
    const bool strict_png_ok = downloaded_id != AV_CODEC_ID_PNG || valid_png_bytes(downloaded);
    const auto converted = strict_png_ok ? convert_to_png(temporary) : std::nullopt;
    const auto dimensions = converted ? png_dimensions(*converted) : std::nullopt;
    if (!dimensions || dimensions->width < full_portrait_width || dimensions->height < full_portrait_height ||
        !replace_atomically(temporary, cache)) {
      fs::remove(temporary, ec);
      mark_remote_failure(failure);
      return {};
    }
    fs::remove(failure, ec);
    return cache;
  }
}  // namespace

namespace platf::steam::artwork {
  std::optional<std::vector<std::uint8_t>> export_png(const fs::path &source) {
    std::error_code ec;
    if (!fs::is_regular_file(source, ec) || fs::file_size(source, ec) > max_remote_bytes || ec) return std::nullopt;
    const auto png = convert_to_png(source);
    if (!png || png->size() > max_remote_bytes) return std::nullopt;
    return png;
  }

  bool import_png(const std::vector<std::uint8_t> &bytes, const fs::path &output) {
    if (bytes.size() > max_remote_bytes || !valid_png_bytes(bytes)) return false;
    const auto dimensions = png_dimensions(bytes);
    if (!dimensions || dimensions->width > 4096 || dimensions->height > 4096) return false;
    std::error_code ec;
    fs::create_directories(output.parent_path(), ec);
    return !ec && write_output(output, bytes);
  }

  fs::path session_cover(std::string_view provider, std::uint64_t id,
                         const std::string &revision, const fs::path &output, session_fetcher_t fetcher) {
#if defined(__linux__)
    if (revision.empty() || id == 0 || (provider != "steam" && provider != "lutris")) return {};
    const auto marker = output.string() + ".session-meta";
    std::ifstream metadata(marker);
    std::string previous;
    std::getline(metadata, previous);
    if (previous == revision && regular(output) && valid_png(output)) return output;
    const auto request = "provider-" + std::string(provider) + "-artwork:" + std::to_string(id);
    std::optional<std::vector<std::uint8_t>> bytes;
    if (fetcher) {
      bytes = fetcher(request);
    } else {
      const auto payload = provider_scan::detail::capture_command(
        "/usr/libexec/vibeshine/vibepollo-session-exec", request,
        {std::chrono::duration_cast<std::chrono::milliseconds>(provider_scan::command_timeout), max_remote_bytes});
      if (payload) bytes.emplace(payload->begin(), payload->end());
    }
    if (bytes && import_png(*bytes, output)) {
      write_output(marker, std::vector<std::uint8_t>(revision.begin(), revision.end()));
      return output;
    }
    // A transient session transition must not discard the last usable cover.
    if (regular(output) && valid_png(output)) return output;
#endif
    return {};
  }

  std::string remote_portrait_url(std::uint32_t app_id) {
    return "https://shared.fastly.steamstatic.com/store_item_assets/steam/apps/" +
           std::to_string(app_id) + "/library_600x900_2x.jpg";
  }

  fs::path remote_cache_path(const fs::path &appdata, std::uint32_t app_id) {
    return appdata / "covers" / ("steam_" + std::to_string(app_id) + "_600x900_2x.jpg");
  }

  fs::path cache_path(const fs::path &appdata, std::uint32_t app_id) {
    return appdata / "covers" / ("steam_" + std::to_string(app_id) + ".png");
  }

  sync_result_t sync_to(const fs::path &source, const fs::path &output) {
    sync_result_t result;
    const auto fp = fingerprint(source);
    if (!fp) {
      std::error_code ec;
      fs::remove(sidecar_path(output), ec);
      return result;
    }
    result.fingerprint = *fp;
    std::error_code ec;
    fs::create_directories(output.parent_path(), ec);
    source_fingerprint_t cached;
    if (regular(output) && valid_png(output) && read_meta(sidecar_path(output), cached) && cached == *fp) {
      result.client_path = output;
      result.reused = true;
      return result;
    }
    const auto converted = convert_to_png(source);
    if (!converted || !write_output(output, *converted)) {
      fs::remove(sidecar_path(output), ec);
      return result;
    }
    result.client_path = output;
    result.converted = true;
    write_meta(sidecar_path(output), *fp);
    return result;
  }

  sync_result_t sync(std::uint32_t app_id, const fs::path &source, const fs::path &appdata) {
    return sync_to(source, cache_path(appdata, app_id));
  }

  void prepare(std::vector<game_t> &games, const fs::path &appdata, remote_fetcher_t fetcher) {
    for (auto &game : games) {
      game.artwork_client_path.clear();
      auto source = !game.artwork_path.empty() ? game.artwork_path : game.boxart_path;
      if (!game.session_artwork_revision.empty()) {
        source = session_cover("steam", game.app_id, game.session_artwork_revision,
                               appdata / "covers" / ("steam_" + std::to_string(game.app_id) + "_local.png"));
      }
      // Steam's local library_600x900.jpg is commonly 300x450. Never upscale
      // it: use the official fixed-origin 600x900 CDN asset when available,
      // retaining the local image as an offline fallback.
      fs::path effective_source = source;
      if (source.empty() || !full_portrait(source)) {
        const auto remote = obtain_remote_portrait(game.app_id, appdata, fetcher);
        if (!remote.empty()) effective_source = remote;
      }
      // Keep the last converted cover through transient Steam cache/CDN failures.
      const auto cached = cache_path(appdata, game.app_id);
      if (regular(cached) && valid_png_bytes(read_bytes(cached))) {
        game.artwork_client_path = cached;
      }
      if (effective_source.empty()) continue;
      if (!game.session_artwork_revision.empty() && effective_source == source) {
        // This PNG was already converted by the session worker. Do not run
        // desktop-user image decoders again inside the machine host.
        game.artwork_client_path = source;
        continue;
      }
      const auto result = sync(game.app_id, effective_source, appdata);
      if (!result.client_path.empty()) {
        game.artwork_client_path = result.client_path;
      }
    }
  }
}  // namespace platf::steam::artwork
