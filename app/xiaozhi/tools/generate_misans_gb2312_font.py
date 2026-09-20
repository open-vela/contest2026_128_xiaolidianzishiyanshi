#!/usr/bin/env python3
"""Generate an LVGL v9 2-bpp bitmap font from all GB2312 characters."""

import argparse
import hashlib
from pathlib import Path

from PIL import ImageFont

GLYPH_ALIASES = {
    # MiSans lacks GB2312's Katakana middle dot. Its Latin middle dot is the
    # same mark, so center that glyph in a full-width cell instead of baking
    # FreeType's missing-glyph box into the bitmap font.
    0x30FB: 0x00B7,
}


def gb2312_codepoints():
    codepoints = set(range(0x20, 0x7F))
    for lead in range(0xA1, 0xF8):
        for trail in range(0xA1, 0xFF):
            try:
                text = bytes((lead, trail)).decode("gb2312")
            except UnicodeDecodeError:
                continue
            if len(text) == 1:
                codepoints.add(ord(text))
    return sorted(codepoints)


def pack_2bpp(pixels):
    packed = bytearray()
    byte = 0
    shift = 6
    for pixel in pixels:
        value = (pixel * 3 + 127) // 255
        byte |= value << shift
        if shift == 0:
            packed.append(byte)
            byte = 0
            shift = 6
        else:
            shift -= 2
    if shift != 6:
        packed.append(byte)
    return packed


def format_array(values, formatter, indent="    ", per_line=12):
    lines = []
    for start in range(0, len(values), per_line):
        chunk = values[start:start + per_line]
        lines.append(indent + ", ".join(formatter(value) for value in chunk) + ",")
    return "\n".join(lines)


def generate(font_path, output_path, size):
    font = ImageFont.truetype(str(font_path), size)
    ascent, descent = font.getmetrics()
    codepoints = gb2312_codepoints()
    bitmap = bytearray()
    descriptors = []

    for codepoint in codepoints:
        render_codepoint = GLYPH_ALIASES.get(codepoint, codepoint)
        character = chr(render_codepoint)
        left, top, right, bottom = font.getbbox(character, anchor="ls")
        mask = font.getmask(character, mode="L")
        mask_width, mask_height = mask.size
        if mask.size != (right - left, bottom - top):
            raise ValueError(f"Pillow mask metrics disagree for U+{codepoint:04X}")
        ink_box = mask.getbbox()
        bitmap_index = len(bitmap)

        if ink_box is None:
            box_width = box_height = 0
            offset_x = offset_y = 0
        else:
            ink_left, ink_top, ink_right, ink_bottom = ink_box
            box_width = ink_right - ink_left
            box_height = ink_bottom - ink_top
            offset_x = left + ink_left
            offset_y = -(top + ink_bottom)
            if codepoint in GLYPH_ALIASES:
                offset_x += round((size - font.getlength(character)) / 2)
            raw = bytes(mask)
            pixels = []
            for y in range(ink_top, ink_bottom):
                row = y * mask_width
                pixels.extend(raw[row + ink_left:row + ink_right])
            bitmap.extend(pack_2bpp(pixels))

        if codepoint in GLYPH_ALIASES:
            advance = size * 16
        else:
            advance = round(font.getlength(character) * 16)
        if not (0 <= bitmap_index < (1 << 20)):
            raise ValueError("bitmap exceeds LV_FONT_FMT_TXT_LARGE=0 limit")
        if not (0 <= advance < (1 << 12)):
            raise ValueError(f"advance out of range for U+{codepoint:04X}")
        if not (-128 <= offset_x <= 127 and -128 <= offset_y <= 127):
            raise ValueError(f"offset out of range for U+{codepoint:04X}")
        descriptors.append((bitmap_index, advance, box_width, box_height,
                            offset_x, offset_y))

    range_start = codepoints[0]
    range_length = codepoints[-1] - range_start + 1
    if range_length > 0xFFFF:
        raise ValueError("GB2312 Unicode span does not fit an LVGL sparse cmap")
    unicode_offsets = [codepoint - range_start for codepoint in codepoints]
    source_hash = hashlib.sha256(font_path.read_bytes()).hexdigest()

    with output_path.open("w", encoding="utf-8", newline="\n") as output:
        output.write(
            "/*******************************************************************************\n"
            " * Generated file: do not edit manually.\n"
            f" * Source: MiSans-Regular.ttf (sha256 {source_hash})\n"
            f" * Characters: GB2312 (7445 double-byte characters) plus ASCII, {len(codepoints)} total\n"
            " * U+30FB uses MiSans U+00B7 centered in a full-width cell.\n"
            f" * Size: {size}px, Bpp: 2, uncompressed\n"
            " *******************************************************************************/\n\n"
            "#include <lvgl/lvgl.h>\n\n"
            "static LV_ATTRIBUTE_LARGE_CONST const uint8_t glyph_bitmap[] = {\n"
        )
        output.write(format_array(bitmap, lambda value: f"0x{value:02x}"))
        output.write("\n};\n\n")

        output.write("static const lv_font_fmt_txt_glyph_dsc_t glyph_dsc[] = {\n")
        output.write("    {0}, /* Glyph ID 0 is reserved. */\n")
        for index, (bitmap_index, advance, box_width, box_height,
                    offset_x, offset_y) in enumerate(descriptors, 1):
            output.write(
                "    {.bitmap_index = %d, .adv_w = %d, .box_w = %d, "
                ".box_h = %d, .ofs_x = %d, .ofs_y = %d}, /* %d */\n"
                % (bitmap_index, advance, box_width, box_height,
                   offset_x, offset_y, index)
            )
        output.write("};\n\n")

        output.write("static const uint16_t unicode_list_0[] = {\n")
        output.write(format_array(unicode_offsets, lambda value: f"0x{value:04x}", per_line=10))
        output.write("\n};\n\n")

        output.write(
            "static const lv_font_fmt_txt_cmap_t cmaps[] = {\n"
            "    {\n"
            f"        .range_start = {range_start}, .range_length = {range_length},\n"
            "        .glyph_id_start = 1, .unicode_list = unicode_list_0,\n"
            f"        .glyph_id_ofs_list = NULL, .list_length = {len(codepoints)},\n"
            "        .type = LV_FONT_FMT_TXT_CMAP_SPARSE_TINY,\n"
            "    },\n"
            "};\n\n"
            "static const lv_font_fmt_txt_dsc_t font_dsc = {\n"
            "    .glyph_bitmap = glyph_bitmap,\n"
            "    .glyph_dsc = glyph_dsc,\n"
            "    .cmaps = cmaps,\n"
            "    .kern_dsc = NULL,\n"
            "    .kern_scale = 0,\n"
            "    .cmap_num = 1,\n"
            "    .bpp = 2,\n"
            "    .kern_classes = 0,\n"
            "    .bitmap_format = LV_FONT_FMT_TXT_PLAIN,\n"
            "};\n\n"
            "const lv_font_t xiaozhi_misans_gb2312_16 = {\n"
            "    .get_glyph_dsc = lv_font_get_glyph_dsc_fmt_txt,\n"
            "    .get_glyph_bitmap = lv_font_get_bitmap_fmt_txt,\n"
            f"    .line_height = {ascent + descent},\n"
            f"    .base_line = {descent},\n"
            "    .subpx = LV_FONT_SUBPX_NONE,\n"
            "    .kerning = LV_FONT_KERNING_NONE,\n"
            "    .underline_position = -2,\n"
            "    .underline_thickness = 1,\n"
            "    .dsc = &font_dsc,\n"
            "    .fallback = LV_FONT_DEFAULT,\n"
            "};\n"
        )

    print(f"generated {output_path}: {len(codepoints)} glyphs, "
          f"{len(bitmap)} bitmap bytes, line height {ascent + descent}")


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--font", required=True, type=Path)
    parser.add_argument("--output", required=True, type=Path)
    parser.add_argument("--size", default=16, type=int)
    args = parser.parse_args()
    generate(args.font, args.output, args.size)


if __name__ == "__main__":
    main()
