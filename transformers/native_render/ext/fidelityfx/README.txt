AMD FidelityFX SDK API headers (FSR SDK v2.4.0, Kits/FidelityFX/api/include and Kits/FidelityFX/upscalers/include),
https://github.com/GPUOpen-LibrariesAndSDKs/FidelityFX-SDK - MIT licence, in each file's header. Flattened into this
folder: ffx_upscale.h includes "ffx_api.h" / "ffx_api_types.h" from here instead of ../../api/include.

The SDK's signed DLLs (the loader and the upscaler) are under AMD's own binary licence and are not in this repository:
transformers/CMakeLists.txt fetches them with their SHA-256 when TF_FSR_SDK is on (build.ps1 -FsrSdk). TODO 299.
