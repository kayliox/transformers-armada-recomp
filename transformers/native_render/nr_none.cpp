// No Direct3D 12 (builds other than Windows): nr_d3d.cpp and nr_glshare.cpp left out. The native renderer never becomes
// ready (drawsPictures false), so the capture keeps the emulated (GS) drawing on, as when a D3D12 device is missing.
// With vkd3d (TF_NATIVE_RENDERER, transformers/CMakeLists.txt) nr_d3d.cpp is built: only the GL interop is stubbed here
// (vkd3d makes no shared handles, so the pictures reach the window through the CPU, as on Windows without interop).
#include "nr_device.h"
#include "nr_glshare.h"
#include "nr_install.h"
#include "nr_types.h"

namespace nr::glshare
{
    bool available() { return false; }
    unsigned import(void *, uint64_t, uint32_t, uint32_t, unsigned &memory) { return memory = 0; }
    void release(unsigned, unsigned) {}
    bool readBack(unsigned, uint32_t, uint32_t, unsigned &maxColour, unsigned &maxAlpha)
    {
        maxColour = maxAlpha = 0u;
        return false;
    }
}

#if !defined(TF_NATIVE_RENDERER)
namespace nr
{
    uint64_t pipelinesMade() { return 0; }
    void preUpload(const std::shared_ptr<const Texture> &) {}
    bool rendererStarting() { return false; }
    int bakeShaders(const char *) { return 1; } // (no native renderer: nothing to bake)

    namespace
    {
        // This build has no native renderer: every call reports the device and pictures absent, so the capture draws
        // with the GS (nr_capture.cpp: drawsPictures false) and no call pretends to have rendered anything.
        class NoneBackend final : public Backend
        {
        public:
            void prepare() override {}
            bool start() override { return false; }
            bool startingNow() override { return false; }
            bool drawsPictures() override { return false; }
            bool deviceLost() override { return false; }
            bool grassGpuReady() override { return false; }
            double waitForFrameSlot() override { return 0.0; }
            void render(const Frame &, const std::string &) override {}
            void preUpload(const std::shared_ptr<const Texture> &) override {}
            bool latestPicture(const uint8_t *&, uint32_t &, int &, uint64_t &, d3d::SharedPicture &) override { return false; }
            void pictureShown(int) override {}
            void setSharedPictures(bool) override {}
            bool latestShared(d3d::SharedPicture &, uint64_t &) override { return false; }
            void sharedHeld(int) override {}
            bool waitPicture(uint64_t, int64_t, int64_t &) override { return false; }
            int pacingMode() override { return 0; }
            void setWindowSize(int, int) override {}
            void setAntiAliasing(int) override {}
            void setSupersampling(bool) override {}
            void setSpeedupsForced(int) override {}
            uint32_t largeImageCap() override { return 0; }
            uint64_t largeImageBudget() override { return 0; }
            uint64_t largeImageBytes() override { return 0; }
            uint64_t adapterMemoryMB() override { return 0; }
            int qualityPreset() override { return 0; }
            double gpuPictureMs() override { return 0.0; }
            double gpuExtraMs() override { return 0.0; }
            double cpuPictureMs() override { return 0.0; }
            double replayWaitMs() override { return 0.0; }
            double slotWaitMs() override { return 0.0; }
            double pacerDelayMs() override { return 0.0; }
        };
    }

    Backend &noneBackend()
    {
        static NoneBackend backend;
        return backend;
    }
}

namespace nr::gfx
{
    namespace
    {
        // The frame builder (nr_frame.cpp) is linked in this build but never runs behind NoneBackend: a call reports
        // no resource and no picture, never a successful draw.
        class NoneDevice final : public Device
        {
        public:
            bool prepareFrame(uint32_t, uint32_t) override { return false; }
            bool beginFrame(uint64_t) override { return false; }
            uint32_t frameSlot() override { return 0; }
            bool endFrame(const FrameEnd &) override { return false; }
            void setFrameDone(std::function<void(uint32_t)>) override {}
            void report(Report) override {}
            uint32_t vendorId() override { return 0; }
            uint64_t videoMemory(bool) override { return 0; }
            TargetId createTarget(const TargetDesc &) override { return 0; }
            bool resizeTarget(TargetId, const TargetDesc &) override { return false; }
            void releaseTarget(TargetId) override {}
            void clearColour(TargetId, const float *) override {}
            void clearDepth(TargetId, float) override {}
            void use(TargetId, UseFor) override {}
            void useDepth(TargetId, DepthUse) override {}
            TextureId view(TargetId, bool) override { return 0; }
            TextureId snapshot(TargetId) override { return 0; }
            TextureId createTexture(uint32_t, uint32_t, Format, uint32_t) override { return 0; }
            bool uploadTexture(TextureId, uint32_t, const void *, uint32_t) override { return false; }
            bool uploadTextureLater(TextureId, std::weak_ptr<const Texture>, bool) override { return false; }
            bool textureReady(TextureId) override { return false; }
            void releaseTexture(TextureId) override {}
            uint32_t textureSlotsFree() override { return 0; }
            uint64_t textureBytes(TextureId) override { return 0; }
            uint32_t createMesh(const void *, uint32_t, const void *, uint32_t) override { return 0; }
            void releaseMesh(uint32_t) override {}
            bool ringAlloc(RingKind, uint32_t, uint32_t, RingSpan &) override { return false; }
            void setTarget(TargetId, bool) override {}
            void setViewport(float, float, float, float) override {}
            void setScissor(const Rect &) override {}
            bool setPipeline(const PipelineDesc &) override { return false; }
            bool effect(Effect) override { return false; }
            void setBlendFactor(const float *) override {}
            void bindTextures(TextureId, TextureId, TextureId) override {}
            void bindDrawConstants(const RingSpan &) override {}
            void setVertices(const RingSpan &, uint32_t) override {}
            void setIndices(const RingSpan &) override {}
            void setPreviousVertices(const RingSpan &) override {}
            void setMesh(uint32_t) override {}
            void draw(uint32_t, uint32_t, uint32_t, uint32_t) override {}
            void drawIndexed(uint32_t, uint32_t, uint32_t, uint32_t) override {}
            void drawIndirect(uint64_t, uint32_t) override {}
            void growGrass(const Frame &) override {}
            bool grassMesh(uint64_t) override { return false; }
            void copyWhole(TargetId, TargetId) override {}
            bool timestamps() override { return false; }
            QueryId timestamp() override { return 0; }
            bool readTimestamps(uint32_t, std::vector<uint64_t> &, uint64_t &) override { return false; }
            bool pipelineStats() override { return false; }
            void beginStats(uint32_t) override {}
            void endStats(uint32_t) override {}
            bool readStats(uint32_t, std::vector<PipelineStats> &) override { return false; }
            bool predication() override { return false; }
            void predicate(bool) override {}
            ReadbackId readTarget(TargetId) override { return 0; }
            bool readback(ReadbackId, Readback &) override { return false; }
            void releaseReadback(ReadbackId) override {}
        };
    }

    Device &noneDevice()
    {
        static NoneDevice device;
        return device;
    }
}
#endif
