// Patch perilaku game: fungsi guest yang ditimpa dengan versi buatan kita.
//
// XenonRecomp menandai setiap fungsi hasil recompile sebagai "weak", jadi mendefinisikan
// PPC_FUNC(sub_XXXXXXXX) di sini otomatis menggantikannya. Fungsi aslinya tetap bisa
// dipanggil lewat __imp__sub_XXXXXXXX.
#include <cstdlib>
#include "kernel.h"

// Implementasi asli (PPC_FUNC_IMPL = extern "C").
extern "C" PPC_FUNC(__imp__sub_82382B00);

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
