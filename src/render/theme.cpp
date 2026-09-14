#include "render/theme.h"

namespace nexus::render {

namespace {

D2D1_COLOR_F FromHex(unsigned char r, unsigned char g, unsigned char b,
                     unsigned char a = 255)
{
    return D2D1::ColorF(r / 255.0f, g / 255.0f, b / 255.0f, a / 255.0f);
}

}  // namespace

const D2D1_COLOR_F Theme::kWindow = FromHex(0x18, 0x18, 0x18);
const D2D1_COLOR_F Theme::kPanel = FromHex(0x20, 0x20, 0x20);
const D2D1_COLOR_F Theme::kCard = FromHex(0x25, 0x25, 0x25);
const D2D1_COLOR_F Theme::kCardHover = FromHex(0x30, 0x30, 0x30);
const D2D1_COLOR_F Theme::kInput = FromHex(0x1D, 0x1D, 0x1D);
const D2D1_COLOR_F Theme::kBorder = FromHex(0x35, 0x35, 0x35);
const D2D1_COLOR_F Theme::kText = FromHex(0xE5, 0xE5, 0xE5);
const D2D1_COLOR_F Theme::kTextSecondary = FromHex(0x99, 0x99, 0x99);
const D2D1_COLOR_F Theme::kAccent = FromHex(0x5B, 0x8D, 0xBE);
const D2D1_COLOR_F Theme::kDanger = FromHex(0xB8, 0x5C, 0x5C);
const D2D1_COLOR_F Theme::kStar = FromHex(0xD9, 0xB4, 0x4A);

}  // namespace nexus::render
