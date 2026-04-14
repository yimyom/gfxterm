/*
 *  gfxterm: a library to display images on the terminal with kitty, sixel, iterm2 protocols and tmux
 *  Copyright (C) 2026 David Bellot <david.bellot@gmail.com>

 *  This program is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation, either version 3 of the License, or
 *  (at your option) any later version.

 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.

 *  You should have received a copy of the GNU General Public License
 *  along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */

#pragma once

#include <cstddef>
#include <cstdint>

#include <algorithm>
#include <optional>
#include <string>
#include <string_view>
#include <vector>
#include <map>
#include <format>

#include <termios.h>
#include <unistd.h>
#include <sys/ioctl.h>

uint32_t generate_unique_id();
constexpr std::string_view control_name(unsigned char c);
std::string to_bracket_notation(std::string_view input);
std::vector<uint8_t> load_binary_file(const std::string& filename);

namespace base64
{
std::vector<std::string_view> chunk(const std::string& encoded, size_t chunk_size = 4096);
std::string encode(void const* src, size_t size);
} // namespace base64

struct StdinGuard
{
    termios original;
    bool valid;

    StdinGuard() : valid(false)
    {
        if(tcgetattr(STDIN_FILENO, &original) == 0)
        {
            valid = true;
        }
    }

    ~StdinGuard()
    {
        if(valid)
        {
            tcsetattr(STDIN_FILENO, TCSAFLUSH, &original);
        }
    }

    StdinGuard(const StdinGuard&) = delete;
    StdinGuard& operator=(const StdinGuard&) = delete;
};

struct Image
{
    uint32_t id = 0;            /// image id

    uint32_t width=0, height=0; /// image dimension in pixels
    uint32_t cols = 0, rows=0;  /// image dimension in characters

    int col=0, row=0;           /// Coordinates at display time
};
using Images = std::map<uint32_t, Image>;

// Custom formatter for Image
template <> struct std::formatter<Image> : std::formatter<std::string>
{
    // Parse format specification (e.g., "width:10") – optional, keep simple
    constexpr auto parse(format_parse_context& ctx)
    {
        // Just accept the empty format specifier (or any string up to '}')
        auto it = ctx.begin();
        while(it != ctx.end() && *it != '}')
            ++it;
        return it; // point to '}'
    }

    // Format the Image into the output context
    auto format(const Image& img, format_context& ctx) const
    {
        // Build a descriptive string
        std::string out;
        out += "{id=" + std::to_string(img.id);
        out += ", size=" + std::to_string(img.width) + "x" + std::to_string(img.height);
        out += ", cell=" + std::to_string(img.cols) + "x" + std::to_string(img.rows);
        out += ", display_pos=(" + std::to_string(img.col) + "," + std::to_string(img.row) + ")";
        out += "}";

        // Delegate formatting of the string to the base formatter
        return std::formatter<std::string>::format(out, ctx);
    }
};

class TerminalGraphics
{
    public:

        TerminalGraphics();

        // Terminal capabilities
        bool has_sixel();
        bool has_iterm2();
        bool has_kitty();
        bool is_in_tmux();
        uint32_t cols();
		uint32_t rows();
        uint32_t width();
		uint32_t height();
        uint32_t block_width();
		uint32_t block_height();

        // Basic terminal functions
        std::optional<winsize> terminal_size();
        std::optional<std::tuple<int,int>> cursor_position();
        void move_cursor(int row, int col);

        // Query
        std::string tmux_wrap(const std::string& query);
        std::optional<std::string> query(const std::string& q, const std::string& st = "\033\\");

        // Image managment
        virtual uint32_t send_image(const std::vector<uint8_t>& png, uint32_t id=0) = 0;
        virtual bool display_at_cursor(uint32_t id, int indent) = 0;
        virtual bool display_at_position(uint32_t id, int row, int col) = 0;
        virtual bool display_again(uint32_t id) = 0;
        virtual void delete_image(uint32_t id) = 0;
        Images& images();

    protected:
        Images _images;

    private:
        bool _tmux;
        bool _has_sixel;
        bool _has_iterm2;
        bool _has_kitty;

        bool check_tmux();
        bool check_sixel();
        bool check_iterm2();
        bool check_kitty();

        uint32_t _cols, _rows;
        uint32_t _width, _height;
        uint32_t _block_width, _block_height;

        using Dimension = std::optional<std::tuple<unsigned int, unsigned int>>;
        Dimension query_size(const std::string& size);
        void get_terminal_size();
        void get_terminal_pixel_dimensions();
        void get_block_size();
};

class KittyGraphics : public TerminalGraphics
{
    public:
        using TerminalGraphics::TerminalGraphics;

        uint32_t send_image(const std::vector<uint8_t>& png, uint32_t id=0) override;
        bool display_at_cursor(uint32_t id, int indent=0) override;
        bool display_at_position(uint32_t id, int row, int col) override;
        bool display_again(uint32_t id) override; 
        void delete_image(uint32_t id) override;

    private:
        std::string send_image_query(const std::vector<std::string_view>& payload, uint32_t id, int cols, int rows);
        std::string diacritic(int value);
        std::string xy_msb(int x, int y, uint8_t msg);
        std::string unicode_placeholders(uint32_t id, int indent, int rows, int cols);
};

class SixelGraphics : public TerminalGraphics
{
    public:
        using TerminalGraphics::TerminalGraphics;

        uint32_t send_image(const std::vector<uint8_t>& png, uint32_t id = 0) override;
        bool display_at_cursor(uint32_t id, int indent = 0) override;
        bool display_at_position(uint32_t id, int row, int col) override;
        bool display_again(uint32_t id) override;
        void delete_image(uint32_t id) override;

    private:
        struct Pixel { uint8_t r, g, b, a; };
        struct RawImage
        {
            uint32_t              width  = 0;
            uint32_t              height = 0;
            std::vector<Pixel>    pixels;   ///< row-major, RGBA
        };
        using Color3   = std::array<uint8_t, 3>;
        using Palette  = std::vector<Color3>;

        std::map<uint32_t, std::string> _sixel_cache;

        std::optional<RawImage> decode_png(const std::vector<uint8_t>& png);
        Palette quantize_colors(const RawImage& img, int max_colors = 256);
        void append_rle(std::string& out, char ch, int count);
        int nearest_color(const Color3& c, std::span<const Color3> palette);

        std::string build_sixel(const RawImage& img);
        bool emit_sixel(uint32_t id, int indent = 0);

    struct Bucket
    {
        std::vector<Color3> colors; ///< unique colours assigned to this bucket

        /// Split along the channel with the greatest value range; return two halves.
        [[nodiscard]] std::pair<Bucket, Bucket> split() const
        {
            // Find the channel with the widest range
            std::array<uint8_t, 3> lo = {255, 255, 255};
            std::array<uint8_t, 3> hi = {0, 0, 0};
            for(const auto& c : colors)
                for(int ch = 0; ch < 3; ++ch)
                {
                    lo[ch] = std::min(lo[ch], c[ch]);
                    hi[ch] = std::max(hi[ch], c[ch]);
                }

            int axis = 0;
            int range = hi[0] - lo[0];
            for(int ch = 1; ch < 3; ++ch)
                if(hi[ch] - lo[ch] > range)
                {
                    range = hi[ch] - lo[ch];
                    axis = ch;
                }

            // Sort along that axis and split at the median
            auto sorted = colors;
            std::ranges::sort(sorted, [axis](const Color3& a, const Color3& b) noexcept { return a[axis] < b[axis]; });

            const size_t mid = sorted.size() / 2;
            Bucket left, right;
            left.colors.assign(sorted.begin(), sorted.begin() + mid);
            right.colors.assign(sorted.begin() + mid, sorted.end());
            return {std::move(left), std::move(right)};
        }

        /// Representative colour: average of all colours in the bucket.
        [[nodiscard]] Color3 average() const noexcept
        {
            if(colors.empty())
                return {0, 0, 0};
            uint32_t r = 0, g = 0, b = 0;
            for(const auto& c : colors)
            {
                r += c[0];
                g += c[1];
                b += c[2];
            }
            const uint32_t n = static_cast<uint32_t>(colors.size());
            return {static_cast<uint8_t>(r / n), static_cast<uint8_t>(g / n), static_cast<uint8_t>(b / n)};
        }
};


};

class ITermGraphics : public TerminalGraphics
{
    public:
        using TerminalGraphics::TerminalGraphics;

        uint32_t send_image(const std::vector<uint8_t>& png, uint32_t id = 0) override;
        bool display_at_cursor(uint32_t id, int indent = 0) override;
        bool display_at_position(uint32_t id, int row, int col) override;
        bool display_again(uint32_t id) override;
        void delete_image(uint32_t id) override;

    private:
        std::map<uint32_t, std::string> _iterm_cache;
        std::string build_osc1337(const std::string& b64,
                                  size_t   byte_size,
                                  uint32_t img_cols,
                                  uint32_t img_rows) noexcept;
        bool emit_iterm(uint32_t id, int indent = 0);
};
