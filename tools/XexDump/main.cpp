// XexDump: membuka default.xex (dekripsi + dekompresi lewat XenonUtils),
// lalu menyimpan "image" game apa adanya di memori Xbox 360 ke file .bin.
// Byte ke-0 file .bin = alamat `base` (biasanya 0x82000000).
//
// Pakai: XexDump <input.xex> <output.bin>

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <fstream>
#include <file.h>
#include <image.h>

int main(int argc, char** argv)
{
    if (argc != 3)
    {
        printf("Usage: XexDump [input XEX file path] [output image file path]\n");
        return 1;
    }

    const auto file = LoadFile(argv[1]);
    if (file.empty())
    {
        printf("Gagal membaca %s\n", argv[1]);
        return 1;
    }

    auto image = Image::ParseImage(file.data(), file.size());
    if (image.data == nullptr)
    {
        printf("Gagal mem-parse XEX\n");
        return 1;
    }

    // Catatan: untuk XEX dengan kompresi "basic", XenonUtils mengisi image.size dengan
    // ukuran dari header keamanan, padahal buffer yang dialokasikan bisa lebih kecil.
    // Jadi kita hanya menulis sampai akhir section terakhir (selalu ada di dalam buffer).
    size_t dumpSize = 0;
    for (const auto& section : image.sections)
    {
        dumpSize = std::max(dumpSize, section.base + section.size - image.base);
    }

    std::ofstream out(argv[2], std::ios::binary);
    out.write(reinterpret_cast<const char*>(image.data.get()), dumpSize);
    out.close();
    if (!out)
    {
        printf("Gagal menulis %s (errno=%d)\n", argv[2], errno);
        return 1;
    }

    printf("base=0x%zX size=0x%X entry=0x%zX\n", image.base, image.size, image.entry_point);
    for (const auto& section : image.sections)
    {
        printf("section %-8s base=0x%zX size=0x%X flags=%s\n", section.name.c_str(), section.base, section.size,
            (section.flags & SectionFlags_Code) ? "code" : "data");
    }
    // Simbol yang dikenali XenonUtils = fungsi import dari kernel/XAM (mis. RtlUnwind).
    for (const auto& symbol : image.symbols)
    {
        printf("symbol 0x%zX %s\n", symbol.address, symbol.name.c_str());
    }
    return 0;
}
