#ifndef NEXUS_SRC_RENDER_THEME_H_
#define NEXUS_SRC_RENDER_THEME_H_

#include <d2d1.h>

namespace nexus::render {

// Dark gray palette from the design doc (section 6).
struct Theme
{
    static const D2D1_COLOR_F kWindow;         // #181818
    static const D2D1_COLOR_F kPanel;          // #202020
    static const D2D1_COLOR_F kCard;           // #252525
    static const D2D1_COLOR_F kCardHover;      // #303030
    static const D2D1_COLOR_F kInput;          // #1D1D1D
    static const D2D1_COLOR_F kBorder;         // #353535
    static const D2D1_COLOR_F kText;           // #E5E5E5
    static const D2D1_COLOR_F kTextSecondary;  // #999999
    static const D2D1_COLOR_F kAccent;         // low saturation blue
    static const D2D1_COLOR_F kDanger;         // low saturation red
    static const D2D1_COLOR_F kStar;           // favorite gold
};

enum class TextRole
{
    kTitle,
    kSecondary,
    kSearch,
    kStatus,
};

}  // namespace nexus::render

#endif  // NEXUS_SRC_RENDER_THEME_H_
