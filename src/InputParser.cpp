
#include "InputParser.h"
#include <charconv>
#include <algorithm>
#include <sstream>
#include <cstring>

namespace {
  const auto default_texture_name = "spright-{0-}.png";

  Definition get_definition(std::string_view command) {
    static const auto s_map = std::map<std::string, Definition, std::less<>>{
      { "texture", Definition::texture },
      { "width", Definition::width },
      { "height", Definition::height },
      { "max-width", Definition::max_width },
      { "max-height", Definition::max_height },
      { "power-of-two", Definition::power_of_two },
      { "square", Definition::square },
      { "align-width", Definition::align_width },
      { "allow-rotate", Definition::allow_rotate },
      { "padding", Definition::padding },
      { "deduplicate", Definition::deduplicate },
      { "alpha", Definition::alpha },
      { "begin", Definition::begin },
      { "path", Definition::path },
      { "sheet", Definition::sheet },
      { "colorkey", Definition::colorkey },
      { "tag", Definition::tag },
      { "grid", Definition::grid },
      { "grid-offset", Definition::grid_offset },
      { "grid-spacing", Definition::grid_spacing },
      { "offset", Definition::offset },
      { "sprite", Definition::sprite },
      { "skip", Definition::skip },
      { "span", Definition::span },
      { "rect", Definition::rect },
      { "pivot", Definition::pivot },
      { "trim", Definition::trim },
      { "trim-threshold", Definition::trim_threshold },
      { "trim-margin", Definition::trim_margin },
      { "extrude", Definition::extrude },
      { "common-divisor", Definition::common_divisor },

      // aliases
      { "in", Definition::sheet },
      { "out", Definition::texture },
    };
    const auto it = s_map.find(command);
    if (it == s_map.end())
      return Definition::none;
    return it->second;
  }

  int index_of(std::string_view string, std::initializer_list<const char*> strings) {
    auto i = 0;
    for (auto s : strings) {
      if (string == s)
        return i;
      ++i;
    }
    return -1;
  }
} // namespace

[[noreturn]] void InputParser::error(std::string message) {
  if (m_line_number)
    message += " in line " + std::to_string(m_line_number);
  throw std::runtime_error(message);
}

void InputParser::check(bool condition, std::string_view message) {
  if (!condition)
    error(std::string(message));
}

TexturePtr InputParser::get_texture(const State& state) {
  auto& texture = m_textures[std::filesystem::weakly_canonical(state.texture)];
  if (!texture) {
    texture = std::make_shared<Texture>(Texture{
      .filename = FilenameSequence(path_to_utf8(state.texture)),
      .width = state.width,
      .height = state.height,
      .max_width = state.max_width,
      .max_height = state.max_height,
      .power_of_two = state.power_of_two,
      .square = state.square,
      .align_width = state.align_width,
      .allow_rotate = state.allow_rotate,
      .border_padding = state.border_padding,
      .shape_padding = state.shape_padding,
      .deduplicate = state.deduplicate,
      .alpha = state.alpha,
      .colorkey = state.alpha_colorkey,
    });
  }
  return texture;
}

ImagePtr InputParser::get_sheet(const std::filesystem::path& path,
    const std::filesystem::path& filename, RGBA colorkey) {
  auto& sheet = m_sheets[std::filesystem::weakly_canonical(path / filename)];
  if (!sheet) {
    auto image = Image(path, filename);

    if (is_opaque(image)) {
      if (!colorkey.a)
        colorkey = guess_colorkey(image);
      replace_color(image, colorkey, RGBA{ });
    }

    sheet = std::make_shared<Image>(std::move(image));
  }
  return sheet;
}

ImagePtr InputParser::get_sheet(const State& state, int index) {
  return get_sheet(state.path,
    state.sheet.get_nth_filename(index),
    state.colorkey);
}

ImagePtr InputParser::get_sheet(const State& state) {
  return get_sheet(state, m_current_sequence_index);
}

void InputParser::sprite_ends(State& state) {
  check(!state.sheet.empty(), "sprite not on sheet");

  // generate rect from grid
  if (empty(state.rect) && !empty(state.grid)) {
    state.rect = {
      m_current_offset.x, m_current_offset.y,
      state.grid.x * state.span.x,
      state.grid.y * state.span.y
    };
    m_current_offset.x += state.grid.x * state.span.x;
  }

  auto sprite = Sprite{ };
  sprite.id = (!state.sprite.empty() ? state.sprite :
    "sprite_" + std::to_string(m_sprites.size()));
  sprite.texture = get_texture(state);
  sprite.source = get_sheet(state);
  sprite.source_rect = (!empty(state.rect) ?
    state.rect : sprite.source->bounds());
  sprite.pivot = state.pivot;
  sprite.pivot_point = state.pivot_point;
  sprite.trim = state.trim;
  sprite.trim_margin = state.trim_margin;
  sprite.trim_threshold = state.trim_threshold;
  sprite.extrude = state.extrude;
  sprite.common_divisor = state.common_divisor;
  sprite.tags = state.tags;
  m_sprites.push_back(std::move(sprite));

  if (state.sheet.is_sequence())
    ++m_current_sequence_index;
  ++m_sprites_in_current_sheet;
}

void InputParser::deduce_sequence_sprites(State& state) {
  auto error = std::error_code{ };
  if (state.sheet.is_infinite_sequence())
    for (auto i = 0; ; ++i)
      if (!std::filesystem::exists(state.path / state.sheet.get_nth_filename(i), error)) {
        state.sheet.set_count(i);
        break;
      }

  for (auto i = 0; i < state.sheet.count(); ++i) {
    const auto& sheet = *get_sheet(state, i);
    state.rect = sheet.bounds();

    if (m_settings.autocomplete) {
      auto& os = m_autocomplete_output;
      os << state.indent << "sprite\n";
    }
    sprite_ends(state);
  }
}

void InputParser::deduce_grid_sprites(State& state) {
  const auto& sheet = *get_sheet(state);
  const auto bounds = get_used_bounds(sheet);

  auto grid = state.grid;
  grid.x += state.grid_spacing.x;
  grid.y += state.grid_spacing.y;

  const auto x0 = floor(bounds.x, grid.x) / grid.x;
  const auto y0 = floor(bounds.y, grid.y) / grid.y;
  const auto x1 = std::min(ceil(bounds.x1(), grid.x), sheet.width()) / grid.x;
  const auto y1 = std::min(ceil(bounds.y1(), grid.y), sheet.height()) / grid.y;

  for (auto y = y0; y < y1; ++y) {
    auto output_offset = false;
    auto skipped = 0;
    for (auto x = x0; x < x1; ++x) {

      state.rect = {
        state.grid_offset.x + x * grid.x,
        state.grid_offset.y + y * grid.y,
        state.grid.x, state.grid.y
      };

      if (is_fully_transparent(sheet, state.rect)) {
        ++skipped;
        continue;
      }

      if (m_settings.autocomplete) {
        auto& os = m_autocomplete_output;
        if (!std::exchange(output_offset, true)) {
          if (x0 || y)
            os << state.indent << "offset " << x0 << " " << y << "\n";
        }

        if (skipped > 0) {
          os << state.indent << "skip";
          if (skipped > 1)
            os << " " << skipped;
          os << "\n";
          skipped = 0;
        }

        os << state.indent << "sprite\n";
      }

      sprite_ends(state);
    }
  }
}

void InputParser::deduce_unaligned_sprites(State& state) {
  const auto& sheet = *get_sheet(state);
  for (const auto& rect : find_islands(sheet)) {
    if (m_settings.autocomplete) {
      auto& os = m_autocomplete_output;
      os << state.indent << "sprite \n";
      if (rect != sheet.bounds())
        os << state.indent << "  rect "
          << rect.x << " " << rect.y << " "
          << rect.w << " " << rect.h << "\n";
    }
    state.rect = rect;
    sprite_ends(state);
  }
}

void InputParser::texture_ends(State& state) {
  get_texture(state);
}

void InputParser::sheet_ends(State& state) {
  if (!m_sprites_in_current_sheet) {
    if (state.sheet.is_sequence()) {
      deduce_sequence_sprites(state);
    }
    else if (!empty(state.grid)) {
      deduce_grid_sprites(state);
    }
    else {
      deduce_unaligned_sprites(state);
    }
  }
  m_sprites_in_current_sheet = { };
}

void InputParser::apply_definition(State& state,
    Definition definition,
    std::vector<std::string_view>& arguments) {

  auto argument_index = 0u;
  const auto arguments_left = [&]() {
    return argument_index < arguments.size();
  };
  const auto check_string = [&]() {
    check(arguments_left(), "invalid argument count");
    return arguments[argument_index++];
  };
  const auto check_path = [&]() {
    return utf8_to_path(check_string());
  };
  const auto is_number_following = [&]() {
    if (!arguments_left())
      return false;
    auto result = 0;
    const auto str = arguments[argument_index];
    return (std::from_chars(str.data(), str.data() + str.size(), result).ec == std::errc());
  };
  const auto check_uint = [&]() {
    auto result = 0;
    auto str = check_string();
    const auto [p, ec] = std::from_chars(str.data(), str.data() + str.size(), result);
    check(ec == std::errc() && result >= 0, "invalid number");
    return result;
  };
  const auto check_bool = [&](bool default_to_true) {
    if (default_to_true && !arguments_left())
      return true;
    const auto str = check_string();
    if (str == "true") return true;
    if (str == "false") return false;
    error("invalid boolean value '" + std::string(str) + "'");
  };
  const auto check_float = [&]() {
    return std::stof(std::string(check_string()));
  };
  const auto check_size = [&](bool default_to_square) {
    const auto x = check_uint();
    return Size{ x, (arguments_left() || !default_to_square ? check_uint() : x) };
  };
  const auto check_rect = [&]() {
    return Rect{ check_uint(), check_uint(), check_uint(), check_uint() };
  };
  const auto check_color = [&]() {
    std::stringstream ss;
    ss << std::hex << check_string();
    check(ss.peek() == '#', "color in HTML notation expected");
    ss.seekg(1);
    auto color = RGBA{ };
    ss >> color.rgba;
    if (!color.a)
      color.a = 255;
    return color;
  };

  switch (definition) {
    case Definition::begin:
      // just for opening scopes, useful for additive definitions (e.g. tags)
      break;

    case Definition::texture:
      state.texture = check_path();
      break;

    case Definition::width:
      state.width = check_uint();
      break;

    case Definition::height:
      state.height = check_uint();
      break;

    case Definition::max_width:
      state.max_width = check_uint();
      break;

    case Definition::max_height:
      state.max_height = check_uint();
      break;

    case Definition::power_of_two:
      state.power_of_two = check_bool(true);
      break;

    case Definition::square:
      state.square = check_bool(true);
      break;

    case Definition::align_width:
      state.align_width = check_uint();
      break;

    case Definition::allow_rotate:
      state.allow_rotate = check_bool(true);
      break;

    case Definition::padding:
      state.shape_padding =
        (arguments_left() ? check_uint() : 1);
      state.border_padding =
        (arguments_left() ? check_uint() : state.shape_padding);
      break;

    case Definition::deduplicate:
      state.deduplicate = check_bool(true);
      break;

    case Definition::alpha: {
      const auto string = check_string();
      if (const auto index = index_of(string, { "keep", "clear", "bleed", "premultiply", "colorkey" }); index >= 0)
        state.alpha = static_cast<Alpha>(index);
      else
        error("invalid alpha value '" + std::string(string) + "'");

      if (state.alpha == Alpha::colorkey)
        state.alpha_colorkey = check_color();
      break;
    }

    case Definition::path:
      state.path = check_path();
      break;

    case Definition::sheet:
      state.sheet = path_to_utf8(check_path());
      m_current_offset = { };
      m_current_sequence_index = 0;
      break;

    case Definition::colorkey:
      state.colorkey = check_color();
      break;

    case Definition::tag: {
      auto& tag = state.tags[std::string(check_string())];
      tag = (arguments_left() ? check_string() : "");
      break;
    }
    case Definition::grid:
      state.grid = check_size(true);
      break;

    case Definition::grid_offset:
      state.grid_offset = check_size(true);
      break;

    case Definition::grid_spacing:
      state.grid_spacing = check_size(true);
      break;

    case Definition::offset:
      check(!empty(state.grid), "offset is only valid in grid");
      m_current_offset.x = static_cast<int>(check_float() * static_cast<float>(state.grid.x));
      m_current_offset.y = static_cast<int>(check_float() * static_cast<float>(state.grid.y));
      break;

    case Definition::skip:
      check(!empty(state.grid), "skip is only valid in grid");
      m_current_offset.x += (arguments_left() ? check_uint() : 1) * state.grid.x;
      break;

    case Definition::span:
      state.span = check_size(false);
      check(state.span.x > 0 && state.span.y > 0, "invalid span");
      break;

    case Definition::sprite:
      if (arguments_left())
        state.sprite = check_string();
      break;

    case Definition::rect:
      state.rect = check_rect();
      break;

    case Definition::pivot:
      if (is_number_following()) {
        state.pivot = { PivotX::custom, PivotY::custom };
        state.pivot_point.x = check_float();
        state.pivot_point.y = check_float();
      }
      else {
        for (auto i = 0; i < 2; ++i) {
          const auto string = check_string();
          if (const auto index = index_of(string, { "left", "center", "right" }); index >= 0)
            state.pivot.x = static_cast<PivotX>(index);
          else if (const auto index = index_of(string, { "top", "middle", "bottom" }); index >= 0)
            state.pivot.y = static_cast<PivotY>(index);
          else
            error("invalid pivot value '" + std::string(string) + "'");
        }
      }
      break;

    case Definition::trim:
      if (!arguments_left()) {
        state.trim = Trim::trim;
      }
      else {
        const auto string = check_string();
        if (const auto index = index_of(string, { "none", "trim", "crop" }); index >= 0)
          state.trim = static_cast<Trim>(index);
        else
          error("invalid trim value '" + std::string(string) + "'");
      }
      break;

    case Definition::trim_margin:
      state.trim_margin = check_uint();
      break;

    case Definition::trim_threshold:
      state.trim_threshold = check_uint();
      check(state.trim_threshold >= 1 && state.trim_threshold <= 255, "invalid threshold");
      break;

    case Definition::extrude:
      state.extrude = (arguments_left() ? check_uint() : 1);
      break;

    case Definition::common_divisor:
      state.common_divisor = check_size(true);
      check(state.common_divisor.x >= 1 && state.common_divisor.y >= 1, "invalid divisor");
      break;

    case Definition::none: break;
  }

  check(!arguments_left(), "invalid argument count");
}

bool InputParser::has_implicit_scope(Definition definition) {
  return (definition == Definition::texture ||
          definition == Definition::sheet ||
          definition == Definition::sprite);
}

void InputParser::scope_ends(State& state) {
  switch (state.definition) {
    case Definition::texture:
      texture_ends(state);
      break;
    case Definition::sheet:
      sheet_ends(state);
      break;
    case Definition::sprite:
      sprite_ends(state);
      break;
    default:
      break;
  }
}

InputParser::InputParser(const Settings& settings)
  : m_settings(settings) {
}

void InputParser::parse(std::istream& input) {
  m_autocomplete_output = { };
  m_sprites_in_current_sheet = { };
  m_current_offset = { };
  m_current_sequence_index = { };

  const auto default_indentation = "  ";
  auto detected_indetation = std::string(default_indentation);
  auto scope_stack = std::vector<State>();
  scope_stack.emplace_back();
  scope_stack.back().level = -1;
  scope_stack.back().texture = default_texture_name;

  auto autocomplete_space = std::stringstream();
  const auto pop_scope_stack = [&](int level) {
    for (auto last = scope_stack.rbegin(); ; ++last) {
      if (has_implicit_scope(last->definition) && level <= last->level) {
        auto& state = scope_stack.back();
        state.definition = last->definition;

        // add indentation before autocompleting in implicit scope
        if (&*last == &scope_stack.back())
          state.indent += detected_indetation;

        scope_ends(state);
      }
      else if (level >= last->level) {
        const auto top = last.base();

        // keep texture set on same level
        if (top != scope_stack.end() &&
            top != scope_stack.begin() &&
            top->definition == Definition::texture)
          std::prev(top)->texture = top->texture;

        scope_stack.erase(top, scope_stack.end());
        return;
      }
    }
  };

  auto buffer = std::string();
  auto arguments = std::vector<std::string_view>();
  for (m_line_number = 1; !input.eof(); ++m_line_number) {
    std::getline(input, buffer);

    auto line = ltrim(buffer);
    if (line.empty() || starts_with(line, "#")) {
      if (m_settings.autocomplete) {
        if (input.eof())
          m_autocomplete_output << autocomplete_space.str();
        else
          autocomplete_space << buffer << '\n';
      }
      continue;
    }

    split_arguments(line, &arguments);
    const auto definition = get_definition(arguments[0]);
    if (definition == Definition::none)
      error("invalid definition '" + std::string(arguments[0]) + "'");
    arguments.erase(arguments.begin());

    const auto level = static_cast<int>(buffer.size() - line.size());
    pop_scope_stack(level);

    if (level > scope_stack.back().level || has_implicit_scope(definition))
      scope_stack.push_back(scope_stack.back());

    auto& state = scope_stack.back();
    state.definition = definition;
    state.level = level;
    state.indent = std::string(begin(buffer), begin(buffer) + level);

    if (detected_indetation.empty() && !state.indent.empty())
      detected_indetation = state.indent;

    apply_definition(state, definition, arguments);

    if (m_settings.autocomplete) {
      const auto space = autocomplete_space.str();
      m_autocomplete_output << space << buffer << '\n';
      autocomplete_space = { };
    }
  }
  pop_scope_stack(-1);
  m_line_number = 0;
}