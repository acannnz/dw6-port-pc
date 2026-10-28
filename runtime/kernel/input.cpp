// Input: controller Xbox di PC (XInput) punya tata letak tombol yang sama dengan Xbox 360,
// jadi statusnya cukup diteruskan. Kalau controller tidak ada, keyboard dipakai sebagai cadangan
// (hanya untuk pemain 1):
//   WASD = stik kiri, panah = stik kanan, Enter = Start, Backspace = Back,
//   J = A, K = B, U = X, I = Y, Q = LB, E = RB, Z = LT, C = RT, 1-4 = D-pad.
// TODO: keyboard dibaca global (GetAsyncKeyState) karena belum ada jendela game.
#define NOMINMAX
#include <windows.h>
#include <xinput.h>
#include <cstring>
#include "../kernel.h"

constexpr uint32_t ERROR_DEVICE_NOT_CONNECTED_X = 1167;

struct Gamepad
{
    uint16_t buttons;
    uint8_t leftTrigger, rightTrigger;
    int16_t thumbLX, thumbLY, thumbRX, thumbRY;
};

static bool KeyDown(int key)
{
    return (GetAsyncKeyState(key) & 0x8000) != 0;
}

static Gamepad ReadKeyboard()
{
    Gamepad pad{};
    const struct { int key; uint16_t button; } buttons[] = {
        { '1', XINPUT_GAMEPAD_DPAD_UP }, { '2', XINPUT_GAMEPAD_DPAD_DOWN },
        { '3', XINPUT_GAMEPAD_DPAD_LEFT }, { '4', XINPUT_GAMEPAD_DPAD_RIGHT },
        { VK_RETURN, XINPUT_GAMEPAD_START }, { VK_BACK, XINPUT_GAMEPAD_BACK },
        { 'Q', XINPUT_GAMEPAD_LEFT_SHOULDER }, { 'E', XINPUT_GAMEPAD_RIGHT_SHOULDER },
        { 'J', XINPUT_GAMEPAD_A }, { 'K', XINPUT_GAMEPAD_B }, { 'U', XINPUT_GAMEPAD_X }, { 'I', XINPUT_GAMEPAD_Y },
    };
    for (const auto& mapping : buttons)
    {
        if (KeyDown(mapping.key))
            pad.buttons |= mapping.button;
    }
    pad.leftTrigger = KeyDown('Z') ? 255 : 0;
    pad.rightTrigger = KeyDown('C') ? 255 : 0;
    pad.thumbLX = int16_t((KeyDown('D') ? 32767 : 0) - (KeyDown('A') ? 32767 : 0));
    pad.thumbLY = int16_t((KeyDown('W') ? 32767 : 0) - (KeyDown('S') ? 32767 : 0));
    pad.thumbRX = int16_t((KeyDown(VK_RIGHT) ? 32767 : 0) - (KeyDown(VK_LEFT) ? 32767 : 0));
    pad.thumbRY = int16_t((KeyDown(VK_UP) ? 32767 : 0) - (KeyDown(VK_DOWN) ? 32767 : 0));
    return pad;
}

// Kembalikan true kalau pemain `user` punya input (controller, atau keyboard untuk pemain 0).
static bool ReadGamepad(uint32_t user, Gamepad& pad, uint32_t& packet)
{
    XINPUT_STATE state{};
    if (user < 4 && XInputGetState(user, &state) == ERROR_SUCCESS)
    {
        const auto& g = state.Gamepad;
        pad = { g.wButtons, g.bLeftTrigger, g.bRightTrigger, g.sThumbLX, g.sThumbLY, g.sThumbRX, g.sThumbRY };
        packet = state.dwPacketNumber;
        return true;
    }
    if (user == 0)
    {
        static uint32_t keyboardPacket = 0;
        pad = ReadKeyboard();
        packet = ++keyboardPacket;
        return true;
    }
    return false;
}

static void StoreGamepad(uint8_t* base, uint32_t address, const Gamepad& pad)
{
    GuestStoreU16(base, address + 0, pad.buttons);
    base[address + 2] = pad.leftTrigger;
    base[address + 3] = pad.rightTrigger;
    GuestStoreU16(base, address + 4, uint16_t(pad.thumbLX));
    GuestStoreU16(base, address + 6, uint16_t(pad.thumbLY));
    GuestStoreU16(base, address + 8, uint16_t(pad.thumbRX));
    GuestStoreU16(base, address + 10, uint16_t(pad.thumbRY));
}

// uint32_t XamInputGetState(uint32_t UserIndex, uint32_t Flags, X_INPUT_STATE* State)
// X_INPUT_STATE: u32 PacketNumber, lalu X_INPUT_GAMEPAD (12 byte)
PPC_FUNC(__imp__XamInputGetState)
{
    Gamepad pad{};
    uint32_t packet = 0;
    if (!ReadGamepad(ctx.r3.u32, pad, packet))
    {
        ctx.r3.u64 = ERROR_DEVICE_NOT_CONNECTED_X;
        return;
    }
    const uint32_t state = ctx.r5.u32;
    if (state != 0)
    {
        GuestStoreU32(base, state, packet);
        StoreGamepad(base, state + 4, pad);
    }
    ctx.r3.u64 = 0;
}

// uint32_t XamInputGetCapabilities(uint32_t UserIndex, uint32_t Flags, X_INPUT_CAPABILITIES* Caps)
// X_INPUT_CAPABILITIES: u8 Type, u8 SubType, u16 Flags, X_INPUT_GAMEPAD (12 byte), u16 vibrasi kiri/kanan
PPC_FUNC(__imp__XamInputGetCapabilities)
{
    Gamepad unused{};
    uint32_t packet = 0;
    if (!ReadGamepad(ctx.r3.u32, unused, packet))
    {
        ctx.r3.u64 = ERROR_DEVICE_NOT_CONNECTED_X;
        return;
    }
    const uint32_t caps = ctx.r5.u32;
    memset(base + caps, 0, 20);
    base[caps + 0] = 1; // XINPUT_DEVTYPE_GAMEPAD
    base[caps + 1] = 1; // XINPUT_DEVSUBTYPE_GAMEPAD
    const Gamepad all{ 0xFFFF, 255, 255, -1, -1, -1, -1 }; // semua tombol & sumbu tersedia
    StoreGamepad(base, caps + 4, all);
    GuestStoreU16(base, caps + 16, 0xFFFF);
    GuestStoreU16(base, caps + 18, 0xFFFF);
    ctx.r3.u64 = 0;
}

// uint32_t XamInputSetState(uint32_t UserIndex, uint32_t Flags, X_INPUT_VIBRATION* Vibration)
PPC_FUNC(__imp__XamInputSetState)
{
    const uint32_t vibration = ctx.r5.u32;
    if (ctx.r3.u32 < 4 && vibration != 0)
    {
        XINPUT_VIBRATION v{ GuestLoadU16(base, vibration), GuestLoadU16(base, vibration + 2) };
        XInputSetState(ctx.r3.u32, &v);
    }
    ctx.r3.u64 = 0;
}
