// The GL side of the shared picture (nr_glshare.h): the D3D12 texture imported as GL memory and a
// texture made on it. The functions come from the driver (wglGetProcAddress), not from raylib's
// loader, so this file needs no GL headers; GL 1.1 entry points come from opengl32.dll.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include "nr_glshare.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <vector>

namespace nr::glshare
{
    namespace
    {
        using GLenum = unsigned int;
        using GLuint = unsigned int;
        using GLint = int;
        using GLsizei = int;
        using GLuint64 = uint64_t;
        using GLubyte = unsigned char;

        constexpr GLenum kExtensions = 0x1F03, kNumExtensions = 0x821D, kTexture2D = 0x0DE1, kRgba8 = 0x8058,
                         kTextureBinding2D = 0x8069, kMinFilter = 0x2801, kMagFilter = 0x2800, kLinear = 0x2601, kWrapS = 0x2802,
                         kWrapT = 0x2803, kClampToEdge = 0x812F, kHandleD3D12Resource = 0x958A, kDedicated = 0x9581;

        struct Gl
        {
            const GLubyte *(APIENTRY *getStringi)(GLenum, GLuint) = nullptr;
            void(APIENTRY *getIntegerv)(GLenum, GLint *) = nullptr;
            GLenum(APIENTRY *getError)() = nullptr;
            void(APIENTRY *genTextures)(GLsizei, GLuint *) = nullptr;
            void(APIENTRY *deleteTextures)(GLsizei, const GLuint *) = nullptr;
            void(APIENTRY *bindTexture)(GLenum, GLuint) = nullptr;
            void(APIENTRY *texParameteri)(GLenum, GLenum, GLint) = nullptr;
            void(APIENTRY *createMemoryObjects)(GLsizei, GLuint *) = nullptr;
            void(APIENTRY *deleteMemoryObjects)(GLsizei, const GLuint *) = nullptr;
            void(APIENTRY *memoryObjectParameteriv)(GLuint, GLenum, const GLint *) = nullptr;
            void(APIENTRY *importMemoryWin32Handle)(GLuint, GLuint64, GLenum, void *) = nullptr;
            void(APIENTRY *texStorageMem2D)(GLenum, GLsizei, GLenum, GLsizei, GLsizei, GLuint, GLuint64) = nullptr;
            void(APIENTRY *getTexImage)(GLenum, GLint, GLenum, GLenum, void *) = nullptr;
            void(APIENTRY *pixelStorei)(GLenum, GLint) = nullptr;
        } gl;

        bool g_checked = false, g_available = false;

        // (opengl32.dll's own wgl functions, looked up too: no link-time dependency on it.)
        HMODULE openGl()
        {
            static HMODULE module = LoadLibraryA("opengl32.dll");
            return module;
        }
        PROC(WINAPI *g_getProcAddress)(LPCSTR) = nullptr;
        HGLRC(WINAPI *g_currentContext)() = nullptr;

        template <typename F> bool load(F &fn, const char *name)
        {
            PROC p = g_getProcAddress ? g_getProcAddress(name) : nullptr;
            // (wglGetProcAddress gives 0, 1, 2, 3 or -1 for what opengl32.dll exports itself.)
            if (!p || p == reinterpret_cast<PROC>(1) || p == reinterpret_cast<PROC>(2) || p == reinterpret_cast<PROC>(3) ||
                p == reinterpret_cast<PROC>(-1))
            {
                p = openGl() ? GetProcAddress(openGl(), name) : nullptr;
            }
            fn = reinterpret_cast<F>(p);
            return p != nullptr;
        }

        bool hasExtension(const char *name)
        {
            GLint n = 0;
            gl.getIntegerv(kNumExtensions, &n);
            for (GLint i = 0; i < n; ++i)
                if (const GLubyte *e = gl.getStringi(kExtensions, static_cast<GLuint>(i)))
                    if (std::strcmp(reinterpret_cast<const char *>(e), name) == 0)
                        return true;
            return false;
        }
    }

    bool available()
    {
        if (g_checked)
            return g_available;
        g_checked = true;
        if (!openGl())
            return false;
        g_getProcAddress = reinterpret_cast<PROC(WINAPI *)(LPCSTR)>(GetProcAddress(openGl(), "wglGetProcAddress"));
        g_currentContext = reinterpret_cast<HGLRC(WINAPI *)()>(GetProcAddress(openGl(), "wglGetCurrentContext"));
        if (!g_getProcAddress || !g_currentContext || !g_currentContext())
            return false;
        bool ok = load(gl.getStringi, "glGetStringi") && load(gl.getIntegerv, "glGetIntegerv") && load(gl.getError, "glGetError") &&
                  load(gl.genTextures, "glGenTextures") && load(gl.deleteTextures, "glDeleteTextures") &&
                  load(gl.bindTexture, "glBindTexture") && load(gl.texParameteri, "glTexParameteri");
        load(gl.getTexImage, "glGetTexImage"); // (the read-back check: readBack)
        load(gl.pixelStorei, "glPixelStorei");
        const bool memory = ok && hasExtension("GL_EXT_memory_object") && hasExtension("GL_EXT_memory_object_win32");
        ok = memory && load(gl.createMemoryObjects, "glCreateMemoryObjectsEXT") && load(gl.deleteMemoryObjects, "glDeleteMemoryObjectsEXT") &&
             load(gl.memoryObjectParameteriv, "glMemoryObjectParameterivEXT") &&
             load(gl.importMemoryWin32Handle, "glImportMemoryWin32HandleEXT") && load(gl.texStorageMem2D, "glTexStorageMem2DEXT");
        g_available = ok;
        std::fprintf(stderr, "[native render] shared picture (D3D12 texture read by GL): %s\n",
                     ok ? "available" : memory ? "functions missing" : "the driver has no EXT_memory_object_win32");
        return ok;
    }

    unsigned import(void *handle, uint64_t size, uint32_t width, uint32_t height, unsigned &memory)
    {
        memory = 0u;
        if (!available() || !handle)
            return 0u;
        while (gl.getError() != 0u)
        {
        }
        GLint bound = 0;
        gl.getIntegerv(kTextureBinding2D, &bound);
        GLuint mem = 0u, tex = 0u;
        gl.createMemoryObjects(1, &mem);
        const GLint dedicated = 1;
        gl.memoryObjectParameteriv(mem, kDedicated, &dedicated);
        gl.importMemoryWin32Handle(mem, size, kHandleD3D12Resource, handle);
        gl.genTextures(1, &tex);
        gl.bindTexture(kTexture2D, tex);
        gl.texStorageMem2D(kTexture2D, 1, kRgba8, static_cast<GLsizei>(width), static_cast<GLsizei>(height), mem, 0u);
        gl.texParameteri(kTexture2D, kMinFilter, static_cast<GLint>(kLinear));
        gl.texParameteri(kTexture2D, kMagFilter, static_cast<GLint>(kLinear));
        gl.texParameteri(kTexture2D, kWrapS, static_cast<GLint>(kClampToEdge));
        gl.texParameteri(kTexture2D, kWrapT, static_cast<GLint>(kClampToEdge));
        gl.bindTexture(kTexture2D, static_cast<GLuint>(bound));
        const GLenum error = gl.getError();
        if (error != 0u)
        {
            std::fprintf(stderr, "[native render] shared picture %ux%u not imported (GL error %04x)\n", width, height, error);
            gl.deleteTextures(1, &tex);
            gl.deleteMemoryObjects(1, &mem);
            return 0u;
        }
        memory = mem;
        return tex;
    }

    bool readBack(unsigned texture, uint32_t width, uint32_t height, unsigned &maxColour, unsigned &maxAlpha)
    {
        maxColour = maxAlpha = 0u;
        if (!available() || !texture || !gl.getTexImage || width == 0u || height == 0u)
            return false;
        while (gl.getError() != 0u)
        {
        }
        std::vector<unsigned char> pixels(static_cast<size_t>(width) * height * 4u);
        GLint bound = 0;
        gl.getIntegerv(kTextureBinding2D, &bound);
        if (gl.pixelStorei)
            gl.pixelStorei(0x0D05 /* GL_PACK_ALIGNMENT */, 4);
        gl.bindTexture(kTexture2D, texture);
        gl.getTexImage(kTexture2D, 0, 0x1908 /* GL_RGBA */, 0x1401 /* GL_UNSIGNED_BYTE */, pixels.data());
        gl.bindTexture(kTexture2D, static_cast<GLuint>(bound));
        if (gl.getError() != 0u)
            return false;
        for (size_t i = 0; i < pixels.size(); i += 4)
        {
            maxColour = std::max<unsigned>(maxColour, std::max({pixels[i], pixels[i + 1], pixels[i + 2]}));
            maxAlpha = std::max<unsigned>(maxAlpha, pixels[i + 3]);
        }
        return true;
    }

    void release(unsigned texture, unsigned memory)
    {
        if (!available())
            return;
        const GLuint t = texture, m = memory;
        if (t)
            gl.deleteTextures(1, &t);
        if (m)
            gl.deleteMemoryObjects(1, &m);
    }
}
