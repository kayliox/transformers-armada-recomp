// The native renderer's graphics backend (docs/RENDERER_BACKEND.md): the API-neutral types the frame builder hands the
// device (a draw's blend, depth test and colour writes, the formats), and the backend's outer calls as one interface -
// the frame's entry, the pictures to the window, the settings, the timing figures (section 2 of the document). The
// frame's recording itself goes through gfx::Device (nr_device.h). Direct3D 12 (nr_d3d.cpp) is the default backend and
// forwards to its nr::d3d functions; the Vulkan one (vulkan/nr_vulkan.cpp, built with TF_VULKAN, in development) is
// chosen with TF_GFX_API=vulkan (nr_backend.cpp).
#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace nr
{
    struct Frame;
    struct Texture;

    namespace d3d
    {
        struct SharedPicture; // (nr_glshare.h: a picture in a texture the window imports)
    }

    namespace gfx
    {
        // A blend factor: the GS's ALPHA register (Cs - Cd) * C + D, with C one of As, Ad, FIX, turned into the GPU's
        // source and destination factors (nr_draw.cpp, blendOf).
        enum class Blend : uint8_t
        {
            Zero,
            One,
            SrcAlpha,
            InvSrcAlpha,
            DestAlpha,
            InvDestAlpha,
            Factor,   // the blend constant (the GS's FIX)
            InvFactor // 1 - the blend constant
        };
        enum class BlendOp : uint8_t
        {
            Add,
            Subtract,    // source - destination
            RevSubtract  // destination - source
        };
        struct BlendState
        {
            Blend src = Blend::One, dst = Blend::Zero;
            BlendOp op = BlendOp::Add;
            bool enabled = false;
            // Cs * As or Cs * FIX (ALPHA's A = Cs, B = 0) made in the pixel shader (1 As, 2 FIX), the source factor then
            // One: the GS's factor goes to 2.0, the GPU's stops at 1.0.
            int premultiply = 0;
        };

        // The depth test: the GS's ZTST (0 never, 1 always, 2 greater or equal, 3 greater - its Z grows toward the eye).
        enum class Compare : uint8_t
        {
            Never,
            Always,
            GreaterEqual,
            Greater
        };

        // The colour channels a draw writes (the GS's FBMSK, a byte per channel): the same bits as Direct3D 12's and
        // Vulkan's (R 1, G 2, B 4, A 8).
        enum WriteMask : uint8_t
        {
            kWriteR = 1u,
            kWriteG = 2u,
            kWriteB = 4u,
            kWriteA = 8u,
            kWriteAll = 15u
        };

        // The formats the backend makes resources in (docs/RENDERER_BACKEND.md, section 3).
        enum class Format : uint8_t
        {
            RGBA8,      // textures, the picture
            BGRA8,      // the PS2's frame buffers as targets
            BC7,        // the large (HD) images
            R32Float,   // depth read as a texture, the occlusion probe
            Depth32Float,
            RG8,        // SMAA's area table
            R8          // SMAA's search table
        };
    }

    // The backend's calls (docs/RENDERER_BACKEND.md, section 2). Threads as there: the render thread records and submits,
    // the window's thread takes pictures, the settings may come from any thread.
    class Backend
    {
    public:
        virtual ~Backend() = default;

        // ---- start and state ----
        virtual void prepare() = 0;            // the device, shaders and pipelines made on a thread of their own
        virtual bool start() = 0;              // the same on the calling thread; false: no backend
        virtual bool startingNow() = 0;        // prepare() still at work
        virtual bool drawsPictures() = 0;      // up and drawing
        virtual bool deviceLost() = 0;         // removed (a GPU hang): nothing more drawn
        virtual bool grassGpuReady() = 0;      // the grass compute pass exists

        // ---- frames (render thread) ----
        virtual double waitForFrameSlot() = 0; // waits for the next frame slot; the ms waited
        virtual void render(const Frame &frame, const std::string &shotPath) = 0;
        virtual void preUpload(const std::shared_ptr<const Texture> &texture) = 0;

        // ---- pictures to the window (window thread) ----
#if defined(__linux__)
        // `pixels` (RGBA8, `rowPitch` bytes a row) stays valid and unchanged - newer pictures are published elsewhere -
        // until pictureShown(index), which gives the picture back once its upload has returned.
        virtual bool latestPicture(const uint8_t *&pixels, uint32_t &rowPitch, int &index, uint64_t &serial, d3d::SharedPicture &meta) = 0;
        virtual void pictureShown(int index) = 0;
#else
        virtual bool latestPicture(std::vector<uint32_t> &pixels, uint32_t &width, uint32_t &height, uint64_t &serial, bool &has3D) = 0;
#endif
        virtual void setSharedPictures(bool on) = 0;
        virtual bool latestShared(d3d::SharedPicture &out, uint64_t &serial) = 0;
        virtual void sharedHeld(int index) = 0;
        virtual bool waitPicture(uint64_t serial, int64_t untilNs, int64_t &lastNs) = 0;
        virtual int pacingMode() = 0;

        // ---- settings (any thread, from the next frame) ----
#if defined(__linux__)
        virtual void setWindowSize(int width, int height) = 0;
#else
        virtual void setWindowLines(int lines) = 0;
#endif
        virtual void setAntiAliasing(int mode) = 0;
        virtual void setSupersampling(bool on) = 0;
        virtual void setSpeedupsForced(int on) = 0;

        // ---- video memory and the quality preset (any thread) ----
        virtual uint32_t largeImageCap() = 0;
        virtual uint64_t largeImageBudget() = 0;
        virtual uint64_t largeImageBytes() = 0;
        virtual uint64_t adapterMemoryMB() = 0;
        virtual int qualityPreset() = 0;

        // ---- timing figures (ms) for the pacer and the stats lines ----
        virtual double gpuPictureMs() = 0;
        virtual double gpuExtraMs() = 0;
        virtual double cpuPictureMs() = 0;
        virtual double replayWaitMs() = 0;
        virtual double slotWaitMs() = 0;
        virtual double pacerDelayMs() = 0;
    };

    // The backend in use (nr_backend.cpp: Direct3D 12; Vulkan with TF_VULKAN and TF_GFX_API=vulkan).
    Backend &backend();
    // True when the backend this build would use is one of its native ones (Direct3D 12 where it is built, or Vulkan
    // when TF_VULKAN is on and TF_GFX_API=vulkan). It says the chosen backend is built in, not that its device is ready:
    // prepare() may still fail, and the emulated GS then draws. Read it after settings.ini is loaded (the first answer
    // is kept).
    bool nativeBackendChosen();
}
