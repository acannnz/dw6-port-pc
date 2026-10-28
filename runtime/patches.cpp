// Patch perilaku game: fungsi guest yang ditimpa dengan versi buatan kita.
//
// XenonRecomp menandai setiap fungsi hasil recompile sebagai "weak", jadi mendefinisikan
// PPC_FUNC(sub_XXXXXXXX) di sini otomatis menggantikannya. Fungsi aslinya tetap bisa
// dipanggil lewat __imp__sub_XXXXXXXX.
#include <cstdlib>
#include "kernel.h"

// Implementasi asli (PPC_FUNC_IMPL = extern "C").
extern "C" PPC_FUNC(__imp__sub_82382B00);
extern "C" PPC_FUNC(__imp__sub_823253B8);

// --- Pemeriksa state block Direct3D -------------------------------------------------
// sub_823253B8(device, stateBlock, 0) memutar daftar fixup di [stateBlock+116]:
// node = { u32 next, u32 count, count x { u32 nilai, u32 alamat } }. Loop-nya hanya berhenti kalau
// indeks == count, jadi node dengan count 0 membuat loop ~4 miliar kali dan menulis ke seluruh
// memori. Di game asli itu tidak terjadi -> data rusak di tempat lain (lihat CLAUDE.md).
// Sementara: cetak isinya sekali, lalu lewati pemanggilan itu.
PPC_FUNC(sub_823253B8)
{
    const uint32_t stateBlock = ctx.r4.u32;
    uint32_t node = stateBlock ? GuestLoadU32(base, stateBlock + 116) : 0;
    for (int depth = 0; node != 0 && depth < 4096; depth++)
    {
        const uint32_t count = GuestLoadU32(base, node + 4);
        if (count == 0 || count > 0x10000)
        {
            static int logged = 0;
            if (logged++ < 5)
            {
                printf("[patch] state block 0x%X: node #%d di 0x%X count=%u (flags [+108]=0x%X) dari lr=0x%llX\n",
                    stateBlock, depth, node, count, GuestLoadU32(base, stateBlock + 108), (unsigned long long)ctx.lr);
                printf("[patch]   isi state block:");
                for (uint32_t k = 0; k < 32; k++)
                    printf(" %08X", GuestLoadU32(base, stateBlock + k * 4));
                printf("\n[patch]   isi node:");
                for (uint32_t k = 0; k < 8; k++)
                    printf(" %08X", GuestLoadU32(base, node + k * 4));
                printf("\n");
                fflush(stdout);
            }
            return;
        }
        node = GuestLoadU32(base, node);
    }
    __imp__sub_823253B8(ctx, base);
}

// --- Lewati video (sementara pemutar video belum bisa jalan) ----------------------
// sub_82382B00(movie, 0, 0) dipanggil tiap frame untuk mengambil frame video berikutnya.
// Hasil 0x16660026 = "video habis": pemanggilnya (sub_82318D18) lalu mematikan flag putar.
// Pipeline video macet (thread pekerja menunggu Start(), lihat CLAUDE.md), jadi untuk
// sekarang setiap video langsung dianggap selesai. DW6_PLAY_MOVIES=1 untuk mencoba memutar.
constexpr uint32_t MOVIE_END_OF_STREAM = 0x16660026;

PPC_FUNC(sub_82382B00)
{
    static const bool playMovies = getenv("DW6_PLAY_MOVIES") != nullptr;
    if (playMovies)
    {
        __imp__sub_82382B00(ctx, base);
        return;
    }
    static bool logged = false;
    if (!logged)
    {
        logged = true;
        printf("[patch] video dilewati (sub_82382B00 -> selesai)\n");
        fflush(stdout);
    }
    ctx.r3.u64 = MOVIE_END_OF_STREAM;
}
