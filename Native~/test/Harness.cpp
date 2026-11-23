// Runs DLSS through Dlss.h on a device that this program creates, without Unity.
//
//     DlssHarness d3d11|d3d12 [frames] [folder of nvngx_dlss.dll]
//
// It fills a color texture with a pattern, evaluates DLAA on it for a number of frames, and reads the output back.
// The exit code is zero when every step succeeded and the output is not blank. The Streamline DLLs must be next to
// the executable. nvngx_dlss.dll must be there too, unless the third argument names the folder that holds it.

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include <windows.h>
#include <d3d11.h>
#include <d3d12.h>
#include <dxgi1_4.h>
#include <wrl/client.h>

#include "Dlss.h"

using Microsoft::WRL::ComPtr;

namespace
{
    constexpr uint32_t Width = 1280;
    constexpr uint32_t Height = 720;
    constexpr uint32_t Viewport = 1;
    constexpr uint32_t DlaaMode = 6;

    // Any GUID identifies a development build to NGX.
    dlss::Application Application = {0, "5a0ad5c3-9d0b-4d6f-9f3b-3e7f0d1c8a11", "harness", nullptr};

    void Log(int32_t type, const char* message)
    {
        const char* level = type == 2 ? "error" : type == 1 ? "warn" : "info";
        std::printf("[sl %s] %s", level, message);
        size_t length = std::strlen(message);
        if (length == 0 || message[length - 1] != '\n')
            std::printf("\n");
    }

    bool Check(const char* step, int32_t result)
    {
        std::printf("%s: %d\n", step, result);
        return result == dlss::Ok;
    }

    bool Check(const char* step, HRESULT result)
    {
        if (FAILED(result))
            std::printf("%s: HRESULT 0x%08lX\n", step, static_cast<unsigned long>(result));
        return SUCCEEDED(result);
    }

    float Halton(uint32_t index, uint32_t radix)
    {
        float result = 0.0f;
        float fraction = 1.0f / static_cast<float>(radix);
        while (index > 0)
        {
            result += static_cast<float>(index % radix) * fraction;
            index /= radix;
            fraction /= static_cast<float>(radix);
        }
        return result;
    }

    // Diagonal stripes, so that the image has edges for DLSS to work on.
    std::vector<uint32_t> Pattern()
    {
        std::vector<uint32_t> pixels(Width * Height);
        for (uint32_t y = 0; y < Height; y++)
        {
            for (uint32_t x = 0; x < Width; x++)
                pixels[y * Width + x] = ((x + 2 * y) / 48) % 2 == 0 ? 0xFF2060E0u : 0xFFE0E0E0u;
        }
        return pixels;
    }

    dlss::Frame NewFrame(uint32_t index)
    {
        dlss::Frame frame{};
        frame.renderWidth = Width;
        frame.renderHeight = Height;
        frame.outputWidth = Width;
        frame.outputHeight = Height;
        frame.jitterX = Halton(index % 8 + 1, 2) - 0.5f;
        frame.jitterY = Halton(index % 8 + 1, 3) - 0.5f;
        frame.motionScaleX = 1.0f;
        frame.motionScaleY = 1.0f;
        frame.flags = dlss::FrameDepthInverted | (index == 0 ? dlss::FrameReset : 0);
        return frame;
    }

    bool Configure()
    {
        dlss::Options options{};
        options.outputWidth = Width;
        options.outputHeight = Height;
        options.mode = DlaaMode;
        uint32_t renderWidth = 0;
        uint32_t renderHeight = 0;
        bool configured = Check("configure", dlss::Configure(Viewport, options, &renderWidth, &renderHeight));
        std::printf("recommended render size: %ux%u\n", renderWidth, renderHeight);
        return configured;
    }

    bool Report(const dlss::Session& session)
    {
        std::printf(
            "session: init %d, device %d, support %d, functions %d, present hook %d\n",
            session.initResult,
            session.deviceResult,
            session.supportResult,
            session.functionsResult,
            session.presentHookResult);
        std::printf(
            "versions: Streamline %u.%u.%u, NGX %u.%u.%u\n",
            session.streamlineVersion[0],
            session.streamlineVersion[1],
            session.streamlineVersion[2],
            session.ngxVersion[0],
            session.ngxVersion[1],
            session.ngxVersion[2]);
        return session.Started() && session.presentHookResult == dlss::Ok;
    }

    bool CheckOutput(const uint8_t* data, size_t rowPitch)
    {
        uint64_t sum = 0;
        for (uint32_t y = 0; y < Height; y++)
        {
            const uint8_t* row = data + y * rowPitch;
            for (uint32_t x = 0; x < Width; x++)
                sum += row[x * 4] + row[x * 4 + 1] + row[x * 4 + 2];
        }
        double mean = static_cast<double>(sum) / (3.0 * Width * Height);
        std::printf("output mean: %.2f (the pattern's mean is about 176)\n", mean);
        return mean > 100.0 && mean < 250.0;
    }

    bool Finish()
    {
        std::printf("video memory: %.1f MB\n", static_cast<double>(dlss::VideoMemory(Viewport)) / (1024.0 * 1024.0));
        bool freed = Check("free", dlss::Free(Viewport));
        // Streamline releases freed resources a few presents later.
        for (int i = 0; i < 8; i++)
            dlss::EndFrame();
        dlss::Stop();
        return freed;
    }

    bool RunD3D11(uint32_t frames)
    {
        ComPtr<ID3D11Device> device;
        ComPtr<ID3D11DeviceContext> context;
        D3D_FEATURE_LEVEL level = D3D_FEATURE_LEVEL_11_0;
        if (!Check("D3D11CreateDevice", D3D11CreateDevice(
                nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, &level, 1, D3D11_SDK_VERSION, &device, nullptr, &context)))
            return false;

        auto create = [&](DXGI_FORMAT format, UINT bind, const void* pixels, UINT pitch, ComPtr<ID3D11Texture2D>& texture)
        {
            D3D11_TEXTURE2D_DESC description{};
            description.Width = Width;
            description.Height = Height;
            description.MipLevels = 1;
            description.ArraySize = 1;
            description.Format = format;
            description.SampleDesc.Count = 1;
            description.BindFlags = bind;
            D3D11_SUBRESOURCE_DATA data{pixels, pitch, 0};
            return device->CreateTexture2D(&description, pixels != nullptr ? &data : nullptr, &texture);
        };

        std::vector<uint32_t> pattern = Pattern();
        std::vector<float> depthValues(Width * Height, 0.5f);
        std::vector<uint16_t> motionValues(Width * Height * 2, 0);
        ComPtr<ID3D11Texture2D> color, depth, motion, output;
        const UINT read = D3D11_BIND_SHADER_RESOURCE;
        const UINT write = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
        if (!Check("color", create(DXGI_FORMAT_R8G8B8A8_UNORM, read, pattern.data(), Width * 4, color))
            || !Check("depth", create(DXGI_FORMAT_R32_FLOAT, read, depthValues.data(), Width * 4, depth))
            || !Check("motion", create(DXGI_FORMAT_R16G16_FLOAT, read, motionValues.data(), Width * 4, motion))
            || !Check("output", create(DXGI_FORMAT_R8G8B8A8_UNORM, write, nullptr, 0, output)))
            return false;

        if (!Report(dlss::Start(device.Get(), dlss::GraphicsApi::Direct3D11, Application, 1, Log)) || !Configure())
            return false;

        for (uint32_t i = 0; i < frames; i++)
        {
            dlss::Frame frame = NewFrame(i);
            frame.color = color.Get();
            frame.depth = depth.Get();
            frame.motion = motion.Get();
            frame.output = output.Get();
            int32_t evaluated = dlss::Evaluate(Viewport, frame, nullptr);
            int32_t ended = dlss::EndFrame();
            if (i == 0 || evaluated != dlss::Ok || ended != dlss::Ok)
                std::printf("frame %u: evaluate %d, end frame %d\n", i, evaluated, ended);
            if (evaluated != dlss::Ok || ended != dlss::Ok)
                return false;
        }

        D3D11_TEXTURE2D_DESC description{};
        output->GetDesc(&description);
        description.BindFlags = 0;
        description.Usage = D3D11_USAGE_STAGING;
        description.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        ComPtr<ID3D11Texture2D> staging;
        if (!Check("staging", device->CreateTexture2D(&description, nullptr, &staging)))
            return false;
        context->CopyResource(staging.Get(), output.Get());
        D3D11_MAPPED_SUBRESOURCE mapped{};
        if (!Check("map", context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped)))
            return false;
        bool written = CheckOutput(static_cast<const uint8_t*>(mapped.pData), mapped.RowPitch);
        context->Unmap(staging.Get(), 0);
        return Finish() && written;
    }

    struct D3D12Queue
    {
        ComPtr<ID3D12Device> device;
        ComPtr<ID3D12CommandQueue> queue;
        ComPtr<ID3D12CommandAllocator> allocator;
        ComPtr<ID3D12GraphicsCommandList> list;
        ComPtr<ID3D12Fence> fence;
        HANDLE fenceEvent = nullptr;
        uint64_t fenceValue = 0;

        bool Create()
        {
            D3D12_COMMAND_QUEUE_DESC description{};
            description.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
            fenceEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
            return Check("D3D12CreateDevice", D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&device)))
                && Check("queue", device->CreateCommandQueue(&description, IID_PPV_ARGS(&queue)))
                && Check("allocator", device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator)))
                && Check("list", device->CreateCommandList(
                    0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), nullptr, IID_PPV_ARGS(&list)))
                && Check("fence", device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)));
        }

        // Runs the recorded commands, waits for the GPU, and opens the list again.
        bool Submit()
        {
            if (!Check("close", list->Close()))
                return false;
            ID3D12CommandList* lists[] = {list.Get()};
            queue->ExecuteCommandLists(1, lists);
            fenceValue++;
            queue->Signal(fence.Get(), fenceValue);
            fence->SetEventOnCompletion(fenceValue, fenceEvent);
            WaitForSingleObject(fenceEvent, INFINITE);
            return Check("device", device->GetDeviceRemovedReason())
                && Check("allocator reset", allocator->Reset())
                && Check("list reset", list->Reset(allocator.Get(), nullptr));
        }
    };

    bool RunD3D12(uint32_t frames)
    {
        D3D12Queue gpu;
        if (!gpu.Create())
            return false;

        D3D12_HEAP_PROPERTIES defaultHeap{D3D12_HEAP_TYPE_DEFAULT};
        auto create = [&](DXGI_FORMAT format, D3D12_RESOURCE_FLAGS flags, D3D12_RESOURCE_STATES resourceState,
                          ComPtr<ID3D12Resource>& texture)
        {
            D3D12_RESOURCE_DESC description{};
            description.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
            description.Width = Width;
            description.Height = Height;
            description.DepthOrArraySize = 1;
            description.MipLevels = 1;
            description.Format = format;
            description.SampleDesc.Count = 1;
            description.Flags = flags;
            return gpu.device->CreateCommittedResource(
                &defaultHeap, D3D12_HEAP_FLAG_NONE, &description, resourceState, nullptr, IID_PPV_ARGS(&texture));
        };

        const D3D12_RESOURCE_STATES read = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
        const D3D12_RESOURCE_STATES write = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
        ComPtr<ID3D12Resource> color, depth, motion, output;
        if (!Check("color", create(DXGI_FORMAT_R8G8B8A8_UNORM, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_COPY_DEST, color))
            || !Check("depth", create(DXGI_FORMAT_R32_FLOAT, D3D12_RESOURCE_FLAG_NONE, read, depth))
            || !Check("motion", create(DXGI_FORMAT_R16G16_FLOAT, D3D12_RESOURCE_FLAG_NONE, read, motion))
            || !Check("output", create(
                DXGI_FORMAT_R8G8B8A8_UNORM, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, write, output)))
            return false;

        // One buffer serves as the upload source for the pattern and, later, as the destination of the readback.
        D3D12_RESOURCE_DESC colorDescription = color->GetDesc();
        D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};
        UINT64 bufferSize = 0;
        gpu.device->GetCopyableFootprints(&colorDescription, 0, 1, 0, &footprint, nullptr, nullptr, &bufferSize);
        D3D12_RESOURCE_DESC bufferDescription{};
        bufferDescription.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        bufferDescription.Width = bufferSize;
        bufferDescription.Height = 1;
        bufferDescription.DepthOrArraySize = 1;
        bufferDescription.MipLevels = 1;
        bufferDescription.SampleDesc.Count = 1;
        bufferDescription.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        D3D12_HEAP_PROPERTIES uploadHeap{D3D12_HEAP_TYPE_UPLOAD};
        D3D12_HEAP_PROPERTIES readbackHeap{D3D12_HEAP_TYPE_READBACK};
        ComPtr<ID3D12Resource> upload, readback;
        if (!Check("upload buffer", gpu.device->CreateCommittedResource(
                &uploadHeap, D3D12_HEAP_FLAG_NONE, &bufferDescription, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                IID_PPV_ARGS(&upload)))
            || !Check("readback buffer", gpu.device->CreateCommittedResource(
                &readbackHeap, D3D12_HEAP_FLAG_NONE, &bufferDescription, D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                IID_PPV_ARGS(&readback))))
            return false;

        std::vector<uint32_t> pattern = Pattern();
        uint8_t* mapped = nullptr;
        if (!Check("map upload", upload->Map(0, nullptr, reinterpret_cast<void**>(&mapped))))
            return false;
        for (uint32_t y = 0; y < Height; y++)
            std::memcpy(mapped + footprint.Offset + y * footprint.Footprint.RowPitch, &pattern[y * Width], Width * 4);
        upload->Unmap(0, nullptr);

        auto transition = [&](ID3D12Resource* resource, D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after)
        {
            D3D12_RESOURCE_BARRIER barrier{};
            barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            barrier.Transition.pResource = resource;
            barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
            barrier.Transition.StateBefore = before;
            barrier.Transition.StateAfter = after;
            gpu.list->ResourceBarrier(1, &barrier);
        };

        D3D12_TEXTURE_COPY_LOCATION texture{};
        texture.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        D3D12_TEXTURE_COPY_LOCATION buffer{};
        buffer.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        buffer.PlacedFootprint = footprint;

        texture.pResource = color.Get();
        buffer.pResource = upload.Get();
        gpu.list->CopyTextureRegion(&texture, 0, 0, 0, &buffer, nullptr);
        transition(color.Get(), D3D12_RESOURCE_STATE_COPY_DEST, read);
        if (!gpu.Submit())
            return false;

        if (!Report(dlss::Start(gpu.device.Get(), dlss::GraphicsApi::Direct3D12, Application, 1, Log)) || !Configure())
            return false;

        for (uint32_t i = 0; i < frames; i++)
        {
            dlss::Frame frame = NewFrame(i);
            frame.color = color.Get();
            frame.depth = depth.Get();
            frame.motion = motion.Get();
            frame.output = output.Get();
            frame.colorState = read;
            frame.depthState = read;
            frame.motionState = read;
            frame.outputState = write;
            int32_t evaluated = dlss::Evaluate(Viewport, frame, gpu.list.Get());
            bool submitted = gpu.Submit();
            int32_t ended = dlss::EndFrame();
            if (i == 0 || evaluated != dlss::Ok || ended != dlss::Ok)
                std::printf("frame %u: evaluate %d, end frame %d\n", i, evaluated, ended);
            if (evaluated != dlss::Ok || ended != dlss::Ok || !submitted)
                return false;
        }

        transition(output.Get(), write, D3D12_RESOURCE_STATE_COPY_SOURCE);
        texture.pResource = output.Get();
        buffer.pResource = readback.Get();
        gpu.list->CopyTextureRegion(&buffer, 0, 0, 0, &texture, nullptr);
        if (!gpu.Submit() || !Check("map readback", readback->Map(0, nullptr, reinterpret_cast<void**>(&mapped))))
            return false;
        bool written = CheckOutput(mapped + footprint.Offset, footprint.Footprint.RowPitch);
        readback->Unmap(0, nullptr);
        return Finish() && written;
    }
}

int main(int argc, char** argv)
{
    if (argc < 2)
    {
        std::printf("usage: DlssHarness d3d11|d3d12 [frames] [folder of nvngx_dlss.dll]\n");
        return 2;
    }

    uint32_t frames = argc > 2 ? static_cast<uint32_t>(std::atoi(argv[2])) : 16;
    std::wstring runtimeDirectory;
    if (argc > 3)
    {
        runtimeDirectory.assign(argv[3], argv[3] + std::strlen(argv[3]));
        Application.runtimeDirectory = runtimeDirectory.c_str();
    }
    bool passed;
    if (std::strcmp(argv[1], "d3d11") == 0)
        passed = RunD3D11(frames);
    else if (std::strcmp(argv[1], "d3d12") == 0)
        passed = RunD3D12(frames);
    else
        return 2;

    std::printf(passed ? "PASSED\n" : "FAILED\n");
    return passed ? 0 : 1;
}
