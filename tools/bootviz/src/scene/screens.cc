#include "src/scene/screens.h"

#include <cstdio>
#include <cstring>
#include <map>
#include <mutex>

#include "data/screens.h"
#include "include/core/SkBitmap.h"
#include "include/core/SkImageInfo.h"

#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#define STBI_NO_STDIO
#include "stb_image.h"

namespace bootviz::scene {

sk_sp<SkImage> ScreenImage(long long cycle) {
  static std::mutex mu;
  static std::map<long long, sk_sp<SkImage>> cache;
  std::lock_guard<std::mutex> lock(mu);
  if (auto it = cache.find(cycle); it != cache.end()) return it->second;

  char name[40];
  std::snprintf(name, sizeof name, "screen_%07lld.png", cycle);
  sk_sp<SkImage> image;
  for (const data::EmbeddedFile& f : data::Files()) {
    if (std::string_view(f.name) != name) continue;
    int w = 0, h = 0, n = 0;
    stbi_uc* px = stbi_load_from_memory(f.data, static_cast<int>(f.size), &w, &h, &n, 1);
    if (!px) break;
    // Average 2x2 blocks first: the desktop's 1-pixel halftone becomes an even
    // grey instead of a moire pattern when the screen is drawn small.
    const int hw = w / 2, hh = h / 2;
    SkBitmap bm;
    bm.allocPixels(SkImageInfo::Make(hw, hh, kGray_8_SkColorType, kOpaque_SkAlphaType));
    for (int y = 0; y < hh; ++y) {
      const stbi_uc* r0 = px + static_cast<size_t>(2 * y) * w;
      const stbi_uc* r1 = r0 + w;
      uint8_t* out = bm.getAddr8(0, y);
      for (int x = 0; x < hw; ++x) {
        out[x] = static_cast<uint8_t>((r0[2 * x] + r0[2 * x + 1] + r1[2 * x] + r1[2 * x + 1] + 2) / 4);
      }
    }
    stbi_image_free(px);
    bm.setImmutable();
    image = bm.asImage()->withDefaultMipmaps();
    break;
  }
  cache[cycle] = image;
  return image;
}

}  // namespace bootviz::scene
