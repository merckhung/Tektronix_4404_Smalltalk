// The recorded Tektronix display bitmaps (data/screens/*.png), decoded once
// into Skia images.
#pragma once

#include "include/core/SkImage.h"

namespace bootviz::scene {

// The display after `cycle` bytecodes at half resolution (2x2 averaged, so
// 512x512 for the 1024x1024 Tektronix bitmap), or nullptr if it was not
// recorded.
sk_sp<SkImage> ScreenImage(long long cycle);

}  // namespace bootviz::scene
