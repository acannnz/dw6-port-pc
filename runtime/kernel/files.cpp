// Sistem file: game membaca file disc (mis. "game:\LINKDATA.BIN") langsung dari ISO.
//
// ISO Xbox 360 memakai filesystem XDVDFS (sama dengan tools/xiso_extract.py): direktori
// berbentuk pohon biner, sektor 2048 byte, dimulai dari offset partisi game.
#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>
#include "../kernel.h"

constexpr uint64_t SECTOR = 2048;
constexpr uint32_t X_STATUS_OBJECT_NAME_NOT_FOUND = 0xC0000034;
constexpr uint32_t X_STATUS_END_OF_FILE = 0xC0000011;
constexpr uint32_t X_STATUS_INVALID_INFO_CLASS = 0xC0000003;

struct IsoEntry
{
    uint64_t offset = 0; // byte di dalam file ISO
    uint64_t size = 0;
    bool directory = false;
};

struct OpenFile
{
    const IsoEntry* entry = nullptr;
    uint64_t position = 0;
};

static std::mutex g_filesLock;
static FILE* g_iso = nullptr;
static uint64_t g_partition = 0;
static std::unordered_map<std::string, IsoEntry> g_entries;     // path huruf kecil, pemisah '\'
static std::unordered_map<uint32_t, std::unique_ptr<OpenFile>> g_open; // objek guest -> file

static std::string ToLower(std::string text)
{
    std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c) { return char(std::tolower(c)); });
    return text;
}

static bool ReadIso(uint64_t offset, void* buffer, size_t size)
{
    _fseeki64(g_iso, int64_t(offset), SEEK_SET);
    return fread(buffer, 1, size, g_iso) == size;
}

static void WalkDirectory(uint64_t sector, uint64_t size, const std::string& prefix)
{
    std::vector<uint8_t> data(size);
    if (size == 0 || !ReadIso(g_partition + sector * SECTOR, data.data(), size))
        return;

    std::vector<uint32_t> todo = { 0 };
    std::vector<bool> seen(size, false);
    while (!todo.empty())
    {
        const uint32_t off = todo.back();
        todo.pop_back();
        if (off + 14 > size || seen[off])
            continue;
        seen[off] = true;
        const uint16_t left = data[off] | (data[off + 1] << 8);
        const uint16_t right = data[off + 2] | (data[off + 3] << 8);
        if (left == 0xFFFF)
            continue; // padding
        const uint32_t start = data[off + 4] | (data[off + 5] << 8) | (data[off + 6] << 16) | (uint32_t(data[off + 7]) << 24);
        const uint32_t length = data[off + 8] | (data[off + 9] << 8) | (data[off + 10] << 16) | (uint32_t(data[off + 11]) << 24);
        const uint8_t attributes = data[off + 12];
        const uint8_t nameLength = data[off + 13];
        const std::string path = prefix + ToLower(std::string(reinterpret_cast<char*>(&data[off + 14]), nameLength));

        IsoEntry entry{ g_partition + uint64_t(start) * SECTOR, length, (attributes & 0x10) != 0 };
        g_entries[path] = entry;
        if (entry.directory)
            WalkDirectory(start, length, path + "\\");
        if (left)
            todo.push_back(left * 4u);
        if (right)
            todo.push_back(right * 4u);
    }
}

bool MountIso(const char* path)
{
    g_iso = fopen(path, "rb");
    if (g_iso == nullptr)
    {
        printf("[file] gagal membuka ISO %s\n", path);
        return false;
    }
    for (const uint64_t partition : { 0x0ull, 0xFD90000ull, 0x2080000ull, 0x18300000ull })
    {
        char magic[20];
        if (ReadIso(partition + 32 * SECTOR, magic, sizeof(magic)) && memcmp(magic, "MICROSOFT*XBOX*MEDIA", 20) == 0)
        {
            uint8_t root[8];
            ReadIso(partition + 32 * SECTOR + 20, root, 8);
            g_partition = partition;
            const uint32_t rootSector = root[0] | (root[1] << 8) | (root[2] << 16) | (uint32_t(root[3]) << 24);
            const uint32_t rootSize = root[4] | (root[5] << 8) | (root[6] << 16) | (uint32_t(root[7]) << 24);
            WalkDirectory(rootSector, rootSize, "");
            g_entries[""] = IsoEntry{ 0, 0, true }; // root
            printf("[file] ISO dimuat: %zu file/folder\n", g_entries.size());
            return true;
        }
    }
    printf("[file] %s bukan ISO Xbox\n", path);
    return false;
}

// "game:\LINKDATA.BIN", "\Device\Cdrom0\x", "d:\x" -> "linkdata.bin" / "x"
static std::string NormalizePath(std::string path)
{
    std::replace(path.begin(), path.end(), '/', '\\');
    path = ToLower(path);
    for (const char* prefix : { "\\device\\cdrom0\\", "\\device\\cdrom0" })
    {
        if (path.rfind(prefix, 0) == 0)
            return path.substr(strlen(prefix));
    }
    const size_t colon = path.find(':');
    if (colon != std::string::npos)
        path = path.substr(colon + 1);
    while (!path.empty() && path.front() == '\\')
        path.erase(path.begin());
    while (!path.empty() && path.back() == '\\')
        path.pop_back();
    return path;
}

static std::string ReadObjectName(uint8_t* base, uint32_t objectAttributes)
{
    // OBJECT_ATTRIBUTES: +0 RootDirectory, +4 ANSI_STRING* ObjectName, +8 Attributes
    const uint32_t name = GuestLoadU32(base, objectAttributes + 4);
    if (name == 0)
        return {};
    const uint16_t length = GuestLoadU16(base, name);
    const uint32_t buffer = GuestLoadU32(base, name + 4);
    return std::string(reinterpret_cast<char*>(base + buffer), length);
}

static void SetIoStatus(uint8_t* base, uint32_t ioStatus, uint32_t status, uint32_t information)
{
    if (ioStatus != 0)
    {
        GuestStoreU32(base, ioStatus + 0, status);
        GuestStoreU32(base, ioStatus + 4, information);
    }
}

static OpenFile* FindFile(uint32_t handle)
{
    const uint32_t object = GuestHandleToObject(handle);
    const auto it = g_open.find(object);
    return it != g_open.end() ? it->second.get() : nullptr;
}

static uint32_t OpenPath(uint8_t* base, uint32_t pHandle, uint32_t objectAttributes, uint32_t ioStatus)
{
    const std::string name = ReadObjectName(base, objectAttributes);
    const std::string path = NormalizePath(name);

    std::lock_guard lock(g_filesLock);
    const auto it = g_entries.find(path);
    if (it == g_entries.end())
    {
        printf("[file] tidak ditemukan: %s\n", name.c_str());
        SetIoStatus(base, ioStatus, X_STATUS_OBJECT_NAME_NOT_FOUND, 0);
        return X_STATUS_OBJECT_NAME_NOT_FOUND;
    }

    const uint32_t object = GuestKernelAlloc(base, 16);
    base[object] = 0xFF; // bukan objek dispatcher
    auto file = std::make_unique<OpenFile>();
    file->entry = &it->second;
    g_open[object] = std::move(file);
    GuestStoreU32(base, pHandle, CreateGuestHandle(object));
    SetIoStatus(base, ioStatus, X_STATUS_SUCCESS, 1); // 1 = FILE_OPENED
    printf("[file] buka: %s (%llu byte)\n", name.c_str(), (unsigned long long)it->second.size);
    return X_STATUS_SUCCESS;
}

// NTSTATUS NtCreateFile(HANDLE* Handle, uint32_t Access, OBJECT_ATTRIBUTES* Attributes, IO_STATUS_BLOCK* IoStatus,
//     int64_t* AllocationSize, uint32_t FileAttributes, uint32_t ShareAccess, uint32_t Disposition, uint32_t Options)
// TODO: menulis file (save data, cache) belum didukung; hanya membuka file disc yang sudah ada.
PPC_FUNC(__imp__NtCreateFile)
{
    ctx.r3.u64 = OpenPath(base, ctx.r3.u32, ctx.r5.u32, ctx.r6.u32);
}

// NTSTATUS NtOpenFile(HANDLE* Handle, uint32_t Access, OBJECT_ATTRIBUTES* Attributes, IO_STATUS_BLOCK* IoStatus,
//     uint32_t ShareAccess, uint32_t OpenOptions)
PPC_FUNC(__imp__NtOpenFile)
{
    ctx.r3.u64 = OpenPath(base, ctx.r3.u32, ctx.r5.u32, ctx.r6.u32);
}

// NTSTATUS NtReadFile(HANDLE Handle, HANDLE Event, void* ApcRoutine, void* ApcContext, IO_STATUS_BLOCK* IoStatus,
//     void* Buffer, uint32_t Length, int64_t* ByteOffset)
PPC_FUNC(__imp__NtReadFile)
{
    const uint32_t event = ctx.r4.u32;
    const uint32_t ioStatus = ctx.r7.u32;
    const uint32_t buffer = ctx.r8.u32;
    const uint32_t length = ctx.r9.u32;
    const uint32_t pOffset = ctx.r10.u32;

    uint32_t status;
    uint32_t transferred = 0;
    {
        std::lock_guard lock(g_filesLock);
        OpenFile* file = FindFile(ctx.r3.u32);
        if (file == nullptr)
        {
            ctx.r3.u64 = X_STATUS_INVALID_HANDLE;
            return;
        }
        if (pOffset != 0)
            file->position = GuestLoadU64(base, pOffset);

        const uint64_t remaining = file->position < file->entry->size ? file->entry->size - file->position : 0;
        transferred = uint32_t(std::min<uint64_t>(remaining, length));
        if (transferred == 0 && length != 0)
        {
            status = X_STATUS_END_OF_FILE;
        }
        else
        {
            for (uint32_t page = buffer & ~0xFFFu; page < buffer + transferred; page += 0x1000)
                GuestCommit(base, page, 0x1000);
            ReadIso(file->entry->offset + file->position, base + buffer, transferred);
            file->position += transferred;
            status = X_STATUS_SUCCESS;
        }
    }

    SetIoStatus(base, ioStatus, status, transferred);
    if (event != 0)
    {
        if (const uint32_t eventObject = GuestHandleToObject(event))
            SignalGuestObject(base, eventObject, 1);
    }
    ctx.r3.u64 = status;
}

// NTSTATUS NtQueryInformationFile(HANDLE Handle, IO_STATUS_BLOCK* IoStatus, void* Info, uint32_t Length, uint32_t Class)
PPC_FUNC(__imp__NtQueryInformationFile)
{
    const uint32_t ioStatus = ctx.r4.u32;
    const uint32_t info = ctx.r5.u32;
    const uint32_t infoClass = ctx.r7.u32;

    std::lock_guard lock(g_filesLock);
    OpenFile* file = FindFile(ctx.r3.u32);
    if (file == nullptr)
    {
        ctx.r3.u64 = X_STATUS_INVALID_HANDLE;
        return;
    }

    const uint64_t size = file->entry->size;
    switch (infoClass)
    {
    case 5: // FileStandardInformation: AllocationSize, EndOfFile, NumberOfLinks, DeletePending, Directory
        GuestStoreU64(base, info + 0, (size + SECTOR - 1) / SECTOR * SECTOR);
        GuestStoreU64(base, info + 8, size);
        GuestStoreU32(base, info + 16, 1);
        base[info + 20] = 0;
        base[info + 21] = file->entry->directory;
        SetIoStatus(base, ioStatus, X_STATUS_SUCCESS, 24);
        break;
    case 14: // FilePositionInformation
        GuestStoreU64(base, info, file->position);
        SetIoStatus(base, ioStatus, X_STATUS_SUCCESS, 8);
        break;
    case 34: // FileNetworkOpenInformation: 4 waktu, AllocationSize, EndOfFile, FileAttributes
        memset(base + info, 0, 0x38);
        GuestStoreU64(base, info + 32, (size + SECTOR - 1) / SECTOR * SECTOR);
        GuestStoreU64(base, info + 40, size);
        GuestStoreU32(base, info + 48, file->entry->directory ? 0x10 : 0x80);
        SetIoStatus(base, ioStatus, X_STATUS_SUCCESS, 0x38);
        break;
    default:
        printf("[file] NtQueryInformationFile class %u belum didukung\n", infoClass);
        SetIoStatus(base, ioStatus, X_STATUS_INVALID_INFO_CLASS, 0);
        ctx.r3.u64 = X_STATUS_INVALID_INFO_CLASS;
        return;
    }
    ctx.r3.u64 = X_STATUS_SUCCESS;
}

// NTSTATUS NtSetInformationFile(HANDLE Handle, IO_STATUS_BLOCK* IoStatus, void* Info, uint32_t Length, uint32_t Class)
PPC_FUNC(__imp__NtSetInformationFile)
{
    std::lock_guard lock(g_filesLock);
    OpenFile* file = FindFile(ctx.r3.u32);
    if (file == nullptr)
    {
        ctx.r3.u64 = X_STATUS_INVALID_HANDLE;
        return;
    }
    if (ctx.r7.u32 == 14) // FilePositionInformation
        file->position = GuestLoadU64(base, ctx.r5.u32);
    else
        printf("[file] NtSetInformationFile class %u diabaikan\n", ctx.r7.u32);
    SetIoStatus(base, ctx.r4.u32, X_STATUS_SUCCESS, 0);
    ctx.r3.u64 = X_STATUS_SUCCESS;
}
