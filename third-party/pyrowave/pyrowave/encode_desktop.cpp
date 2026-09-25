#define INITGUID
#define WIN32_LEAN_AND_MEAN
#include "dxgi1_5.h"
#include "d3d11_4.h"
#include "volk.h"
#include "pyrowave.h"
#include "logging.hpp"
#include "com_ptr.hpp"
#include "yuv4mpeg.hpp"
#include "cli_parser.hpp"
#include <stdexcept>
#include <vector>
#include "path_utils.hpp"

#define ASSERT_THAT(x) do { \
	if (!(x)) { LOGE("Fatal error executing %s at line %d.\n", #x, __LINE__); std::terminate(); } \
} while(false)

#define CHECK_HRESULT(x) ASSERT_THAT(SUCCEEDED(x))

#define CHECKED(x) do { \
	pyrowave_result _res = x; \
	if (_res != PYROWAVE_SUCCESS) { LOGE("Got pyrowave result %d while executing %s at line %d.\n", _res, #x, __LINE__); std::terminate(); } \
} while(false)

static void print_help()
{
	LOGE("Usage: pyrowave-encode-desktop [out-path.y4m] [--width W] [--height H] [--frames N] [--size <bytes per frame>]\n");
}

int main(int argc, char **argv)
{
	uint32_t out_width = 1280;
	uint32_t out_height = 720;
	uint32_t frames = 10;
	uint32_t payload_size = 500000;
	std::string out_path;

	Util::CLICallbacks cbs;
	cbs.add("--width", [&](Util::CLIParser &parser) { out_width = parser.next_uint(); });
	cbs.add("--height", [&](Util::CLIParser &parser) { out_height = parser.next_uint(); });
	cbs.add("--frames", [&](Util::CLIParser &parser) { frames = parser.next_uint(); });
	cbs.add("--size", [&](Util::CLIParser &parser) { payload_size = parser.next_uint(); });
	cbs.default_handler = [&](const char *arg) { out_path = arg; };

	Util::CLIParser parser(std::move(cbs), argc - 1, argv + 1);
	if (!parser.parse())
	{
		print_help();
		return EXIT_FAILURE;
	}
	else if (parser.is_ended_state())
	{
		print_help();
		return EXIT_SUCCESS;
	}

	if (out_path.empty())
	{
		LOGE("Must provide an output path.\n");
		return EXIT_FAILURE;
	}

	ComPtr<IDXGIFactory1> factory;
	ComPtr<IDXGIAdapter> adapter;
	ComPtr<ID3D11Device> device;
	ComPtr<ID3D11Device5> device5;
	ComPtr<ID3D11DeviceContext> context;
	ComPtr<ID3D11DeviceContext4> context4;
	CHECK_HRESULT(CreateDXGIFactory1(IID_IDXGIFactory, factory.ppv()));
	CHECK_HRESULT(factory->EnumAdapters(0, (IDXGIAdapter **)adapter.ppv()));

	HRESULT hr = D3D11CreateDevice(adapter.get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr, 0 /*D3D11_CREATE_DEVICE_DEBUG*/, nullptr, 0, D3D11_SDK_VERSION,
			(ID3D11Device **)device.ppv(), nullptr, (ID3D11DeviceContext **)context.ppv());
	ASSERT_THAT(SUCCEEDED(hr));
	CHECK_HRESULT(device->QueryInterface(IID_ID3D11Device5, device5.ppv()));
	CHECK_HRESULT(context->QueryInterface(IID_ID3D11DeviceContext4, context4.ppv()));

	ComPtr<ID3D11Fence> share_fence;
	CHECK_HRESULT(device5->CreateFence(0, D3D11_FENCE_FLAG_SHARED, IID_ID3D11Fence, share_fence.ppv()));
	HANDLE fence_handle;
	CHECK_HRESULT(share_fence->CreateSharedHandle(nullptr, GENERIC_ALL, nullptr, &fence_handle));

	ComPtr<IDXGIOutput> dxgi_output;
	ComPtr<IDXGIOutput1> dxgi_output1;
	CHECK_HRESULT(adapter->EnumOutputs(0, (IDXGIOutput **)dxgi_output.ppv()));
	CHECK_HRESULT(dxgi_output->QueryInterface(IID_IDXGIOutput1, dxgi_output1.ppv()));

	ComPtr<IDXGIOutputDuplication> output_duplication;
	CHECK_HRESULT(dxgi_output1->DuplicateOutput(device.get(), (IDXGIOutputDuplication **)output_duplication.ppv()));

	DXGI_OUTDUPL_DESC desc;
	output_duplication->GetDesc(&desc);
	LOGI("Got output desc: %u x %u (%.3f Hz)\n",
			desc.ModeDesc.Width, desc.ModeDesc.Height,
			float(desc.ModeDesc.RefreshRate.Numerator) / float(desc.ModeDesc.RefreshRate.Denominator));

	YUV4MPEGFile y4m;
	char y4m_params[256];
	snprintf(y4m_params, sizeof(y4m_params), "W%u H%u F10:1 Ip A1:1 C420 XYSCSS=420 XCOLORRANGE=FULL\n", out_width, out_height);
	if (!y4m.open_write(out_path, y4m_params))
	{
		LOGE("Failed to open y4m for writing.\n");
		return EXIT_FAILURE;
	}

	pyrowave_device pyro_device;
	pyrowave_encoder encoder;
	pyrowave_decoder decoder;
	pyrowave_image pyro_image;
	pyrowave_sync_object pyro_sync;

	DXGI_ADAPTER_DESC adapter_desc;
	adapter->GetDesc(&adapter_desc);

	LOGI("Device name: %s\n", Granite::Path::to_utf8(adapter_desc.Description).c_str());

	CHECKED(pyrowave_create_device_by_compat(0, 0, 0, 0,
				reinterpret_cast<const pyrowave_luid *>(&adapter_desc.AdapterLuid), &pyro_device));

	pyrowave_encoder_create_info encoder_info = {};
	encoder_info.chroma = PYROWAVE_CHROMA_SUBSAMPLING_420;
	encoder_info.width = out_width;
	encoder_info.height = out_height;
	encoder_info.device = pyro_device;

	pyrowave_decoder_create_info decoder_info = {};
	decoder_info.chroma = PYROWAVE_CHROMA_SUBSAMPLING_420;
	decoder_info.width = out_width;
	decoder_info.height = out_height;
	decoder_info.device = pyro_device;

	CHECKED(pyrowave_encoder_create(&encoder_info, &encoder));
	CHECKED(pyrowave_decoder_create(&decoder_info, &decoder));

	pyrowave_image_create_info image_info = {};
	VkImageCreateInfo image_create_info = { VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
	image_info.device = pyro_device;
	image_info.handle_type = VK_EXTERNAL_MEMORY_HANDLE_TYPE_D3D11_TEXTURE_BIT;
	image_info.image_create_info = &image_create_info;

	image_create_info.imageType = VK_IMAGE_TYPE_2D;
	image_create_info.extent = { out_width, out_height, 1 };
	image_create_info.format = VK_FORMAT_B8G8R8A8_UNORM;
	image_create_info.mipLevels = 1;
	image_create_info.samples = VK_SAMPLE_COUNT_1_BIT;
	image_create_info.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
	image_create_info.tiling = VK_IMAGE_TILING_OPTIMAL;
	image_create_info.arrayLayers = 1;
	image_create_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

	pyrowave_sync_object_create_info sync_create_info = {};
	sync_create_info.device = pyro_device;
	sync_create_info.external_handle = (pyrowave_os_handle)fence_handle;
	sync_create_info.handle_type = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_D3D11_FENCE_BIT;
	sync_create_info.semaphore_type = VK_SEMAPHORE_TYPE_TIMELINE;
	CHECKED(pyrowave_sync_object_create(&sync_create_info, &pyro_sync));

	std::vector<std::vector<uint8_t>> encoded_frames;
	uint64_t timeline = 0;

	for (uint32_t i = 0; i < frames; i++)
	{
		DXGI_OUTDUPL_FRAME_INFO frame_info = {};
		ComPtr<IDXGIResource> resource;

		// MSDN docs recommend to minimize the time between releasing and acquiring next frame.
		if (i != 0)
			output_duplication->ReleaseFrame();
		HRESULT hr = output_duplication->AcquireNextFrame(500, &frame_info, (IDXGIResource **)resource.ppv());

		if (hr == DXGI_ERROR_INVALID_CALL)
		{
			LOGE("Invalid call somehow?\n");
			break;
		}

		if (hr != S_OK)
		{
			LOGE("Failed to acquire next desktop frame, hr #%x.\n", (int)hr);
			break;
		}

		ComPtr<ID3D11Texture2D> tex;
		CHECK_HRESULT(resource->QueryInterface(IID_ID3D11Texture2D, tex.ppv()));

		D3D11_TEXTURE2D_DESC tex_desc;
		tex->GetDesc(&tex_desc);
		LOGI("Got texture: %u x %u (fmt #%x)\n", tex_desc.Width, tex_desc.Height, tex_desc.Format);

		if (tex_desc.Format != DXGI_FORMAT_B8G8R8A8_UNORM)
		{
			LOGE("Spec says only BGRA8 is returned.\n");
			break;
		}

		if (tex_desc.Width != desc.ModeDesc.Width || tex_desc.Height != desc.ModeDesc.Height)
			LOGW("Mismatch in desktop mode vs captured texture ... ?\n");

		ComPtr<IDXGIResource1> resource1;
		CHECK_HRESULT(resource->QueryInterface(IID_IDXGIResource1, resource1.ppv()));

		HANDLE shared_handle;
		CHECK_HRESULT(resource1->CreateSharedHandle(nullptr, GENERIC_ALL, nullptr, &shared_handle));
		image_create_info.extent.width = tex_desc.Width;
		image_create_info.extent.height = tex_desc.Height;
		image_info.external_handle = (pyrowave_os_handle)shared_handle;
		CHECKED(pyrowave_image_create(&image_info, &pyro_image));

		// Unclear if we need to synchronize here, but given it's D3D11, we might
		// have to convert implicit sync into explicit sync, who knows ...
		context4->Signal(share_fence.get(), ++timeline);

		pyrowave_gpu_sync_operation acquire, release;
		pyrowave_scaled_encode_info info = {};
		pyrowave_rate_control rate_control = { payload_size };

		pyrowave_gpu_external_reference external_ref;

		acquire = {};
		acquire.sync.semaphore = pyrowave_sync_object_get_semaphore(pyro_sync);
		acquire.sync.value = timeline;
		acquire.num_images = 1;
		acquire.images = &external_ref;

		release = {};
		release.sync.semaphore = pyrowave_sync_object_get_semaphore(pyro_sync);
		release.sync.value = ++timeline;

		CHECKED(pyrowave_image_get_image_view(pyro_image, VK_IMAGE_ASPECT_COLOR_BIT,
				VK_IMAGE_USAGE_SAMPLED_BIT, &info.view));
		external_ref.image = pyro_image;
		external_ref.queue_family_index = VK_QUEUE_FAMILY_EXTERNAL;

		info.input_color_space = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
		info.output_color_space = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
		info.ycbcr_chroma_midpoint = 128.0f / 255.0f;
		info.intermediate_plane_format = VK_FORMAT_R8_UNORM;

		CHECKED(pyrowave_encoder_encode_gpu_scaled_synchronous(encoder, &acquire, &release, &info, &rate_control));

		std::vector<uint8_t> bitstream(rate_control.maximum_bitstream_size);
		pyrowave_packet packet = {};
		size_t num_packets;
		CHECKED(pyrowave_encoder_packetize(encoder, &packet, rate_control.maximum_bitstream_size, &num_packets, bitstream.data(), bitstream.size()));
		ASSERT_THAT(num_packets == 1);
		ASSERT_THAT(packet.offset == 0);
		bitstream.resize(packet.size);
		encoded_frames.push_back(std::move(bitstream));

		context4->Wait(share_fence.get(), timeline);

		pyrowave_image_destroy(pyro_image);
	}

	output_duplication = {};

	std::unique_ptr<uint8_t[]> y, cb, cr;
	y.reset(new uint8_t[out_width * out_height]);
	cb.reset(new uint8_t[out_width * out_height / 4]);
	cr.reset(new uint8_t[out_width * out_height / 4]);

	for (auto &frame : encoded_frames)
	{
		CHECKED(pyrowave_decoder_push_packet(decoder, frame.data(), frame.size()));
		ASSERT_THAT(pyrowave_decoder_decode_is_ready(decoder, false));

		pyrowave_cpu_buffer cpu_buffer = {};
		cpu_buffer.data[0] = y.get();
		cpu_buffer.data[1] = cb.get();
		cpu_buffer.data[2] = cr.get();
		cpu_buffer.width = out_width;
		cpu_buffer.height = out_height;
		cpu_buffer.format = PYROWAVE_CPU_BUFFER_FORMAT_YUV420P;
		for (int i = 0; i < 3; i++)
		{
			cpu_buffer.plane_size_in_bytes[i] = (out_width * out_height) >> (i ? 2 : 0);
			cpu_buffer.row_stride_in_bytes[i] = out_width >> (i ? 1 : 0);
		}
		CHECKED(pyrowave_decoder_decode_cpu_buffer_synchronous(decoder, &cpu_buffer));

		if (!y4m.begin_frame())
		{
			LOGE("Failed to begin frame.\n");
			return EXIT_FAILURE;
		}

		for (int i = 0; i < 3; i++)
		{
			if (!y4m.write(cpu_buffer.data[i], cpu_buffer.plane_size_in_bytes[i]))
			{
				LOGE("Failed to write plane.\n");
				return EXIT_FAILURE;
			}
		}
	}

	pyrowave_encoder_destroy(encoder);
	pyrowave_decoder_destroy(decoder);
	pyrowave_sync_object_destroy(pyro_sync);
	pyrowave_device_destroy(pyro_device);
}
