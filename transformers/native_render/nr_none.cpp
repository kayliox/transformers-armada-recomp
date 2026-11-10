// No Direct3D 12 (builds other than Windows): nr_d3d.cpp and nr_glshare.cpp left out. The native renderer never becomes
// ready (drawsPictures false), so the capture keeps the emulated (GS) drawing on, as when a D3D12 device is missing.
// With vkd3d (TF_NATIVE_RENDERER, transformers/CMakeLists.txt) nr_d3d.cpp is built: only the GL interop is stubbed here
// (vkd3d makes no shared handles, so the pictures reach the window through the CPU, as on Windows without interop).
#include "nr_glshare.h"
#include "nr_install.h"
#include "nr_types.h"

namespace nr::glshare
{
    bool available() { return false; }
    unsigned import(void *, uint64_t, uint32_t, uint32_t, unsigned &memory) { return memory = 0; }
    void release(unsigned, unsigned) {}
}

#if !defined(TF_NATIVE_RENDERER)
namespace nr
{
    uint64_t pipelinesMade() { return 0; }
    void setTextureSharpness(float) {}
    void preUpload(const std::shared_ptr<const Texture> &) {}
    bool rendererStarting() { return false; }
    int bakeShaders(const char *) { return 1; } // (no native renderer: nothing to bake)
}

namespace nr::d3d
{
    void setWindowSize(int, int) {}
    uint32_t largeImageCap() { return 0; }
    uint64_t largeImageBudget() { return 0; }
    uint64_t largeImageBytes() { return 0; }
    void setAntiAliasing(int) {}
    void setSupersampling(bool) {}
    double gpuPictureMs() { return 0.0; }
    double gpuExtraMs() { return 0.0; }
    double cpuPictureMs() { return 0.0; }
    double replayWaitMs() { return 0.0; }
    double slotWaitMs() { return 0.0; }
    double pacerDelayMs() { return 0.0; }
    bool latestPicture(std::vector<uint32_t> &, uint32_t &, uint32_t &, uint64_t &, bool &) { return false; }
    void prepare() {}
    bool drawsPictures() { return false; }
    double waitForFrameSlot() { return 0.0; }
    bool grassGpuReady() { return false; }
    void render(const Frame &, const std::string &) {}
    void setSpeedupsForced(int) {}
    void setSharedPictures(bool) {}
    bool latestShared(SharedPicture &, uint64_t &) { return false; }
    void sharedHeld(int) {}
    bool waitPicture(uint64_t, int64_t, int64_t &) { return false; }
    int pacingMode() { return 0; }
}
#endif
