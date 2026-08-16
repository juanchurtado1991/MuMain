#pragma once

#include "stdafx.h"

#include <coreclr_delegates.h>

#include "PacketFunctions_ChatServer.h"
#include "PacketFunctions_ConnectServer.h"
#include "PacketFunctions_ClientToServer.h"

#include <cwchar>

#ifdef _WIN32
#include "Core/Platform/WinCompat.h"
#define symLoad GetProcAddress
#else
#include "dlfcn.h"
#include <unistd.h>   // readlink
#include <climits>    // PATH_MAX
#include <cstdint>
#include <string>
#if defined(__APPLE__)
#include <mach-o/dyld.h>  // _NSGetExecutablePath
#endif
#define symLoad dlsym
#endif

// Construct-on-first-use: the library handle is loaded lazily on first access.
// The inline dotnet_* symbol globals (defined in other translation units) load
// through this handle during their own dynamic initialization, so a plain inline
// global here would risk a static-initialization-order fiasco. A function-local
// static is initialized on first call instead, which is well-defined. The macro
// keeps every existing call site (`munique_client_library_handle`) unchanged.
#ifdef _WIN32
inline HINSTANCE get_munique_client_library_handle()
{
    static const HINSTANCE handle = LoadLibrary(L"MUnique.Client.Library.dll");
    return handle;
}
#else
inline void* get_munique_client_library_handle()
{
    // Native AOT emits a platform-native shared library next to the executable:
    // .so on Linux, .dylib on macOS. Resolve the executable directory and load
    // by absolute path so cwd does not matter; fall back to the loader path.
    // Not const-qualified return: dlsym() takes a non-const void* handle.
    static void* const handle = []() -> void* {
#if defined(__APPLE__)
        constexpr const char* kLibName = "MUnique.Client.Library.dylib";
#else
        constexpr const char* kLibName = "MUnique.Client.Library.so";
#endif
        std::string dir;
#if defined(__APPLE__)
        char exe[4096];
        uint32_t size = static_cast<uint32_t>(sizeof(exe));
        if (_NSGetExecutablePath(exe, &size) == 0)
        {
            char real[PATH_MAX];
            if (::realpath(exe, real) != nullptr)
                dir = real;
            else
                dir = exe;
        }
#else
        char exe[4096];
        const ssize_t n = ::readlink("/proc/self/exe", exe, sizeof(exe) - 1);
        if (n > 0)
            dir.assign(exe, static_cast<size_t>(n));
#endif
        if (!dir.empty())
        {
            const std::string::size_type slash = dir.find_last_of('/');
            if (slash != std::string::npos)
            {
                dir.resize(slash + 1);
                dir += kLibName;
                if (void* h = dlopen(dir.c_str(), RTLD_LAZY))
                    return h;
            }
        }
        return dlopen(kLibName, RTLD_LAZY);
    }();
    return handle;
}
#endif
#define munique_client_library_handle get_munique_client_library_handle()

namespace DotNetBridge
{
void ReportDotNetError(const char* detail);
bool IsManagedLibraryAvailable();

template<typename T>
T LoadManagedSymbol(const char* name)
{
    if (!IsManagedLibraryAvailable())
    {
        return nullptr;
    }

    const auto symbol = reinterpret_cast<T>(symLoad(munique_client_library_handle, name));
    if (!symbol)
    {
        ReportDotNetError(name);
    }

    return symbol;
}
}

using DotNetBridge::LoadManagedSymbol;

class Connection
{
private:
    static void OnPacketReceivedS(int32_t handle, int32_t size, BYTE* data);
    static void OnDisconnectedS(int32_t handle);

    PacketFunctions_ChatServer* _chatServer = { };
    PacketFunctions_ConnectServer* _connectServer = { };
    PacketFunctions_ClientToServer* _gameServer = { };

    int32_t _handle;
    void(*_packetHandler)(int32_t, const BYTE*, int32_t);

    void OnDisconnected();
    void OnPacketReceived(const BYTE* data, const int32_t length);

public:
    Connection(const wchar_t* host, int32_t port, bool isEncrypted, void(*packetHandler)(int32_t, const BYTE*, int32_t));
    ~Connection();

    bool IsConnected();
    void Send(const BYTE* data, const int32_t length);
    void Close();

    int32_t GetHandle() const { return _handle; }

    PacketFunctions_ChatServer* ToChatServer() const { return _chatServer; }
    PacketFunctions_ConnectServer* ToConnectServer() const { return _connectServer; }
    PacketFunctions_ClientToServer* ToGameServer() const { return _gameServer; }
};
