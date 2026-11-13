AMD FidelityFX SDK API headers (FSR SDK v2.4.0, Kits/FidelityFX/api/include and Kits/FidelityFX/upscalers/include),
https://github.com/GPUOpen-LibrariesAndSDKs/FidelityFX-SDK - MIT licence, in each file's header. Flattened into this
folder: ffx_upscale.h includes "ffx_api.h" / "ffx_api_types.h" from here instead of ../../api/include.

The SDK's signed DLLs (the loader and the upscaler) are not in this repository; the SDK's docs/license.md lists them
among the files under its MIT terms (both of its notices are in THIRD_PARTY_NOTICES.md). transformers/CMakeLists.txt
fetches them with their SHA-256 in every build (TF_FSR_SDK, on by default; build.ps1 -NoFsrSdk leaves them out). TODO 299.
