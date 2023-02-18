
#include "output.h"

namespace spright {

namespace {
  const Image* get_source(const Sprite& sprite, int map_index) {
    if (map_index < 0)
      return sprite.source.get();
    const auto index = to_unsigned(map_index);
    if (sprite.maps && index < sprite.maps->size())
      return sprite.maps->at(index).get();
    return nullptr;
  }

  bool has_rect_vertices(const Sprite& sprite) {
    const auto& v = sprite.vertices;
    const auto [w, h] = sprite.trimmed_rect.size();
    return (v.size() == 4 &&
      v[0] == PointF(0, 0) &&
      v[1] == PointF(w, 0) &&
      v[2] == PointF(w, h) &&
      v[3] == PointF(0, h));
  }

  bool copy_sprite(Image& target, const Sprite& sprite, int map_index) try {
    const auto source = get_source(sprite, map_index);
    if (!source)
      return false;

    if (sprite.rotated) {
      if (has_rect_vertices(sprite)) {
        copy_rect_rotated_cw(*source, sprite.trimmed_source_rect,
          target, sprite.trimmed_rect.x, sprite.trimmed_rect.y);
      }
      else {
        copy_rect_rotated_cw(*source, sprite.trimmed_source_rect,
          target, sprite.trimmed_rect.x, sprite.trimmed_rect.y, sprite.vertices);
      }
    }
    else {
      if (has_rect_vertices(sprite)) {
        copy_rect(*source, sprite.trimmed_source_rect,
          target, sprite.trimmed_rect.x, sprite.trimmed_rect.y);
      }
      else {
        copy_rect(*source, sprite.trimmed_source_rect,
          target, sprite.trimmed_rect.x, sprite.trimmed_rect.y, sprite.vertices);
      }
    }

    if (sprite.extrude.count) {
      const auto left = (sprite.source_rect.x0() == sprite.trimmed_source_rect.x0());
      const auto top = (sprite.source_rect.y0() == sprite.trimmed_source_rect.y0());
      const auto right = (sprite.source_rect.x1() == sprite.trimmed_source_rect.x1());
      const auto bottom = (sprite.source_rect.y1() == sprite.trimmed_source_rect.y1());
      if (left || top || right || bottom) {
        auto rect = sprite.trimmed_rect;
        if (sprite.rotated)
          std::swap(rect.w, rect.h);
        extrude_rect(target, rect, 
          sprite.extrude.count, sprite.extrude.mode, 
          left, top, right, bottom);
      }
    }
    return true;
  }
  catch (const std::exception& ex) {
#if defined(NDEBUG)
    throw;
#else
    std::fprintf(stderr, "copying sprite '%s' failed: %s\n", 
      sprite.id.c_str(), ex.what());
    return false;
#endif
  }

  void process_alpha(Image& target, const Texture& texture) {
    switch (texture.output->alpha) {
      case Alpha::keep:
        break;

      case Alpha::clear:
        clear_alpha(target);
        break;

      case Alpha::bleed:
        bleed_alpha(target);
        break;

      case Alpha::premultiply:
        premultiply_alpha(target);
        break;

      case Alpha::colorkey:
        make_opaque(target, texture.output->colorkey);
        break;
    }
  }
} // namespace

Image get_output_texture(const Texture& texture, int map_index) {
  auto target = Image(texture.width, texture.height, RGBA{ });

  auto copied_sprite = false;
  for (const auto& sprite : texture.sprites)
    copied_sprite |= copy_sprite(target, sprite, map_index);
  if (!copied_sprite)
    return { };

  process_alpha(target, texture);

  return target;
}

} // namespace