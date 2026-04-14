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

#include <array>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <exception>
#include <fstream>
#include <iostream>
#include <print>
#include <random>
#include <regex>
#include <span>
#include <thread>

#include "gfxterm.h"

#ifndef STB_IMAGE_IMPLEMENTATION
#define STB_IMAGE_IMPLEMENTATION
#endif
// Suppress warnings that stb_image triggers on modern compilers
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#pragma GCC diagnostic ignored "-Wsign-compare"
#include "stb_image.h"
#pragma GCC diagnostic pop

namespace base64
{

std::vector<std::string_view> chunk(const std::string& encoded, size_t chunk_size)
{
    if(encoded.empty() || chunk_size == 0)
    {
        return {};
    }

    std::vector<std::string_view> chunks;
    chunks.reserve((encoded.size() + chunk_size - 1) / chunk_size);

    for(size_t i = 0; i < encoded.size(); i += chunk_size)
    {
        size_t len = std::min(chunk_size, encoded.size() - i);
        chunks.emplace_back(encoded.data() + i, len);
    }

    return chunks;
}

std::string encode(void const* src, std::size_t size)
{
    constexpr char tab[] = {"ABCDEFGHIJKLMNOP"
                            "QRSTUVWXYZabcdef"
                            "ghijklmnopqrstuv"
                            "wxyz0123456789+/"};
    std::size_t out_len = ((size + 2) / 3) * 4;
    std::string encoded(out_len, '\0');

    char* out = encoded.data();
    char const* in = static_cast<char const*>(src);

    for(auto n = size / 3; n--;)
    {
        *out++ = tab[(in[0] & 0xfc) >> 2];
        *out++ = tab[((in[0] & 0x03) << 4) + ((in[1] & 0xf0) >> 4)];
        *out++ = tab[((in[2] & 0xc0) >> 6) + ((in[1] & 0x0f) << 2)];
        *out++ = tab[in[2] & 0x3f];
        in += 3;
    }

    switch(size % 3)
    {
    case 2:
        *out++ = tab[(in[0] & 0xfc) >> 2];
        *out++ = tab[((in[0] & 0x03) << 4) + ((in[1] & 0xf0) >> 4)];
        *out++ = tab[(in[1] & 0x0f) << 2];
        *out++ = '=';
        break;

    case 1:
        *out++ = tab[(in[0] & 0xfc) >> 2];
        *out++ = tab[((in[0] & 0x03) << 4)];
        *out++ = '=';
        *out++ = '=';
        break;

    case 0:
        break;
    }

    return encoded;
}

} // namespace base64

/**
 * Generates a 32‑bit unsigned integer identifier that is unique within the
 * current program run and has a very high probability of being unique across
 * multiple runs.
 *
 * The function is thread‑safe.
 */
uint32_t generate_unique_id()
{
    // Random base, initialised once at program start.
    static const uint32_t base = [] {
        // Combine several entropy sources for a high‑quality seed.
        std::random_device rd;
        auto now = std::chrono::steady_clock::now().time_since_epoch().count();
        std::seed_seq seq{rd(), static_cast<uint32_t>(now), static_cast<uint32_t>(now >> 32),
                          static_cast<uint32_t>(std::hash<std::thread::id>{}(std::this_thread::get_id()))};
        std::mt19937 gen(seq);
        std::uniform_int_distribution<uint32_t> dist;
        return dist(gen);
    }();

    // Atomic counter increments on each call.
    static std::atomic<uint32_t> counter{0};

    // Adding the counter to the base yields a unique value for this run.
    // Wraparound is harmless – uniqueness holds modulo 2^32.
    return base + counter.fetch_add(1, std::memory_order_relaxed);
}

/**
 * Returns the bracket notation name for a given ASCII control character.
 * Example: 0x1B -> "ESC", 0x0A -> "LF", 0x7F -> "DEL".
 */
constexpr std::string_view control_name(unsigned char c)
{
    // Names for 0x00 .. 0x1F (standard abbreviations)
    constexpr std::array<std::string_view, 32> names = {
        "NUL", "SOH", "STX", "ETX", "EOT", "ENQ", "ACK", "BEL", "BS",  "HT", "LF",  "VT",  "FF", "CR", "SO", "SI",
        "DLE", "DC1", "DC2", "DC3", "DC4", "NAK", "SYN", "ETB", "CAN", "EM", "SUB", "ESC", "FS", "GS", "RS", "US"};

    if(c < 0x20)
    {
        // Replace "HT" with "TAB", "LF" stays "LF", etc. (optional aliases)
        if(c == 0x09)
            return "TAB";
        if(c == 0x0A)
            return "LF";
        if(c == 0x0C)
            return "FF";
        if(c == 0x0D)
            return "CR";
        if(c == 0x0B)
            return "VT";
        return names[c];
    }
    if(c == 0x7F)
    {
        return "DEL";
    }
    return {}; // not a control character
}

// Helper: is this Unicode code point printable?
// (Printable: letters, digits, punctuation, symbols, spaces (including non‑breaking),
//  but not control, format, surrogate, private use, non‑character, etc.)
bool is_unicode_printable(char32_t cp)
{
    // Surrogates and non‑characters
    if((cp >= 0xD800 && cp <= 0xDFFF) || cp > 0x10FFFF)
        return false;
    // Cc, Cf, Cs, Cn, Co (control, format, surrogate, unassigned, private‑use)
    if((cp >= 0x0000 && cp <= 0x001F) || (cp >= 0x007F && cp <= 0x009F))
        return false; // Cc
    if((cp >= 0x2000 && cp <= 0x200F) || (cp >= 0x2028 && cp <= 0x202F) || (cp >= 0x2060 && cp <= 0x206F) ||
       (cp >= 0xFFF0 && cp <= 0xFFFF))
        return false; // Cf, Cn (partial)
    if(cp == 0xFEFF || cp == 0xFFFE || cp == 0xFFFF)
        return false;
    // Private Use Area – not printable for debugging (kitty placeholders often live here)
    if((cp >= 0xE000 && cp <= 0xF8FF) || (cp >= 0xF0000 && cp <= 0xFFFFF) || (cp >= 0x100000 && cp <= 0x10FFFF))
        return false;
    // Zero‑width joiners, directional marks, etc.
    if(cp == 0x200B || cp == 0x200C || cp == 0x200D || cp == 0xFEFF)
        return false;
    return true;
}

// Decode one UTF‑8 character from a byte iterator; advances iterator.
// Returns U+FFFD on invalid input or end‑of‑string.
char32_t decode_utf8(const char*& ptr, const char* end)
{
    if(ptr >= end)
        return U'�';
    unsigned char c = *ptr;
    if(c < 0x80)
    {
        // ASCII handled elsewhere, but for completeness
        return c;
    }
    int len;
    char32_t cp;
    if((c & 0xE0) == 0xC0)
    {
        len = 2;
        cp = c & 0x1F;
    }
    else if((c & 0xF0) == 0xE0)
    {
        len = 3;
        cp = c & 0x0F;
    }
    else if((c & 0xF8) == 0xF0)
    {
        len = 4;
        cp = c & 0x07;
    }
    else
    {
        ptr++;
        return U'�';
    } // invalid start byte

    if(ptr + len > end)
        return U'�';
    for(int i = 1; i < len; ++i)
    {
        unsigned char b = ptr[i];
        if((b & 0xC0) != 0x80)
            return U'�';
        cp = (cp << 6) | (b & 0x3F);
    }
    ptr += len - 1; // advance to last byte (loop will ++ptr)
    return cp;
}

std::string to_bracket_notation(std::string_view input)
{
    std::string result;
    result.reserve(input.size() * 4);
    const char* ptr = input.data();
    const char* end = ptr + input.size();
    while(ptr < end)
    {
        unsigned char c = *ptr;
        // 1. Printable ASCII (space .. '~')
        if(c >= 0x20 && c <= 0x7E)
        {
            result.push_back(c);
            ++ptr;
        }
        // 2. Control characters (0x00–0x1F, 0x7F)
        else if(c < 0x20 || c == 0x7F)
        {
            std::string_view name = control_name(c); // your existing function
            result.push_back('<');
            result.append(name);
            result.push_back('>');
            ++ptr;
        }
        // 3. UTF‑8 sequence (c >= 0x80)
        else
        {
            // Remember start of the sequence
            const char* start = ptr;
            char32_t cp = decode_utf8(ptr, end);
            if(is_unicode_printable(cp))
            {
                // Keep the original bytes (they form a printable Unicode char)
                result.append(start, ptr - start + 1);
            }
            else
            {
                // Replace with dummy displayable character
                // Use '?' or U+FFFD as preferred
                result.append("�"); // or "?" if you want ASCII only
            }
            ++ptr; // move past the last byte of the sequence
        }
    }
    return result;
}

std::vector<uint8_t> load_binary_file(const std::string& filename)
{
    std::ifstream file(filename, std::ios::binary | std::ios::ate);

    if(!file.is_open())
    {
        return {}; // return empty vector on failure
    }

    const auto size = file.tellg();
    file.seekg(0, std::ios::beg);

    std::vector<uint8_t> buffer(size);
    file.read(reinterpret_cast<char*>(buffer.data()), size);

    return buffer;
}

// --------------------------------
// TerminalGraphics implementation
// --------------------------------

TerminalGraphics::TerminalGraphics() : 
    _tmux(check_tmux()),
    _has_sixel(check_sixel()),
    _has_iterm2(check_iterm2()),
    _has_kitty(check_kitty())
{
    get_terminal_size();
    get_terminal_pixel_dimensions();
    get_block_size();
}

bool TerminalGraphics::is_in_tmux()
{
    return _tmux;
}

bool TerminalGraphics::has_sixel()
{
    return _has_sixel;
}

bool TerminalGraphics::has_iterm2()
{
    return _has_iterm2;
}

bool TerminalGraphics::has_kitty()
{
    return _has_kitty;
}

bool TerminalGraphics::check_sixel()
{
    std::string q{"\x1b[c"};
    std::regex re("(\\d+)");

    if(auto resp = query(_tmux ? tmux_wrap(q) : q, ";c"); resp)
        for(auto it = std::sregex_iterator(begin(*resp), end(*resp), re); it != std::sregex_iterator(); ++it)
            if(std::smatch(*it).str() == "4")
                return true;
    return false;
}

bool TerminalGraphics::check_iterm2()
{
    auto env = [](const char* name) -> std::string {
        const char* v = std::getenv(name);
        return v ? std::string{v} : std::string{};
    };

    auto term_prog = env("TERM_PROGRAM");
    for(size_t i=0; i<term_prog.size(); i++)
        term_prog[i] = std::tolower(term_prog[i]);

    if(term_prog.contains("iterm.app")
       || term_prog.contains("wezterm")
       || term_prog.contains("hyper"))
        return true;

    if(env("LC_TERMINAL") == "iTerm2")
        return true;

    std::string q("\033[>0q\033[c");
    auto resp = query(q, "c");
    if(resp and (resp->contains("iTerm2") or resp->contains("WezTerm")))
        return true;

    return false;
}

bool TerminalGraphics::check_kitty()
{
    std::string q{"\x1b_Gi=3141592653,s=1,v=1,a=q,t=d,f=24;MARS\x1b\\\x1b[c"};
    auto resp = query(_tmux ? tmux_wrap(q) : q);
    return resp and resp->find("i=3141592653;OK") != std::string::npos;
}

bool TerminalGraphics::check_tmux()
{
    return std::getenv("TMUX") != nullptr;
}

uint32_t TerminalGraphics::cols()
{
    return _cols;
}

uint32_t TerminalGraphics::rows()
{
    return _rows;
}

uint32_t TerminalGraphics::width()
{
    return _width;
}

uint32_t TerminalGraphics::height()
{
    return _height;
}

uint32_t TerminalGraphics::block_width()
{
    return _block_width;
}

uint32_t TerminalGraphics::block_height()
{
    return _block_height;
}

TerminalGraphics::Dimension TerminalGraphics::query_size(const std::string& size)
{
    auto q = size;

    if(is_in_tmux())
        q = tmux_wrap(q);

    auto resp = query(q, "t");

    if(resp and resp->size())
    {
        if(auto h = resp->find_first_of(';') + 1; h != resp->npos)   // start of height after first semi-colon
        {
            if(auto w = resp->find_first_of(';', h) + 1; w != resp->npos and w > h)// start of width after second semi-colon
            {
                try
                {
                    auto height = std::stoul(resp->substr(h, w - h - 1));
                    auto width = std::stoul(resp->substr(w, resp->size() - w - 1));
                    return {{height, width}};
                }
                catch(std::exception& e)
                {
                    // XXX need a better solution here
                    return std::nullopt;
                }
            }
        }
    }

    return std::nullopt;
}

void TerminalGraphics::get_terminal_size()
{
    // with ESC[18t
    auto siz = query_size("\x1b[18t");
    if(siz)
    {
        _rows = std::get<0>(*siz);
        _cols = std::get<1>(*siz);
        return;
    }

    // with TIOCGWINSZ
    struct winsize ws;
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) != -1)
    {
        _rows = ws.ws_row;
        _cols = ws.ws_col;
        return;
    }

    // with stty
    FILE* pipe;
    pipe = popen("stty size 2>/dev/null", "r");
    if(pipe and fscanf(pipe, "%d %d", &_rows, &_cols) == 2)
    {
        pclose(pipe);
        return;
    }

    // with tput
    FILE* pipe_row = popen("tput lines 2>/dev/null", "r");
    FILE* pipe_col = popen("tput cols 2>/dev/null", "r");
    if(pipe_row and pipe_col)
    {
        fscanf(pipe_row, "%d", &_rows);
        fscanf(pipe_col, "%d", &_cols);
        pclose(pipe_row);
        pclose(pipe_col);
        return;
    }

    // default values
    _rows = 80;
    _cols = 24;
}

void TerminalGraphics::get_terminal_pixel_dimensions()
{
    // with TIOCGWINSZ
    struct winsize ws;
    if(ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) != -1)
    {
        _width = ws.ws_xpixel;
        _height = ws.ws_ypixel;
        return ;
    }

    // with escape sequence
    auto siz = query_size("\x1b[14t");
    if(siz)
    {
        _height = std::get<0>(*siz);
        _width = std::get<1>(*siz);
        return;
    }

    // default values
    _height = _width = 0;
}

void TerminalGraphics::get_block_size()
{
    auto siz = query_size("\x1b[16t");

    if(siz)
    {
        _block_height = std::get<0>(*siz);
        _block_width = std::get<1>(*siz);
    }
    else
    {
        _block_height = static_cast<uint32_t>(_height / _cols);
        _block_width = static_cast<uint32_t>(_width / _rows);
    }
}

std::optional<std::tuple<int, int>> TerminalGraphics::cursor_position()
{
    const std::string q("\033[6n");
    auto resp = query(q, "R");

    if(resp)
    {
        int row = 1, col = 1;
        if(sscanf(resp->c_str(), "\033[%d;%dR", &row, &col) == 2)
            return {{row, col}};
    }

    return std::nullopt;
}

void TerminalGraphics::move_cursor(int row, int col)
{
    std::print("\033[{};{}H", row, col);
    std::cout.flush(); // ensure the cursor moves immediately
}

std::string TerminalGraphics::tmux_wrap(const std::string& query)
{
    const std::string TMUX_START_PASSTHROUGH("\033Ptmux;");
    const std::string TMUX_END_PASSTHROUGH("\033\\");
    std::string newq;
    newq.reserve(query.size() * 2);

    for(unsigned char c : query)
    {
        newq.push_back(c);
        if(c == 033)
            newq.push_back(c);
    }

    return TMUX_START_PASSTHROUGH + newq + TMUX_END_PASSTHROUGH;
}

std::optional<std::string> TerminalGraphics::query(const std::string& q, const std::string& st)
{
    StdinGuard guard;
    const size_t max_capacity = 256;

    if(guard.valid and !q.empty() and !st.empty())
    {
        struct termios raw{guard.original};
        raw.c_lflag &= ~(ICANON | ECHO); // disable echo and line buffering
        raw.c_cc[VMIN] = 0;
        raw.c_cc[VTIME] = 1;
        if(tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw) == -1)
            return std::nullopt;

        if(int nb_written = write(STDOUT_FILENO, q.data(), q.size()); nb_written == q.size())
        {
            std::array<std::string::value_type, max_capacity> buf;
            std::string response;
            response.reserve(max_capacity);
            ssize_t r = 0;
            do
            {
                r = read(STDIN_FILENO, buf.data(), max_capacity);
                if(r == -1 or r == 0)
                    break;
                else
                {
                    response.insert(end(response), begin(buf), begin(buf) + r);
                    if(r == max_capacity)
                        response.reserve(response.size() + max_capacity);
                }
            } while(r >= 1 and !(response.size() >= st.size() and
                                 response.compare(response.size() - st.size(), st.size(), st) == 0));
            if(r >= 0)
                return response;
        }
    }

    return std::nullopt;
}

Images& TerminalGraphics::images()
{
    return _images;
}

// --------------------------------
// KittyGraphics implementation
// --------------------------------
std::string KittyGraphics::send_image_query(const std::vector<std::string_view>& payload, uint32_t id, int cols,
                                            int rows)
{
    std::string q;
    auto Siz = payload.size();

    for(size_t i = 0; i < Siz; i++)
    {
        std::string msg = "\x1b_G";
        // Header
        if(i == 0) // first chunk
        {
            msg += std::format("a=t,f=100,i={}", id, cols, rows);
            if(Siz > 1)
                msg += ",m=1";
        }
        else if(Siz > 1 and i < Siz - 1) // next chunks except last
        {
            msg += "m=1";
        }
        else if(i == Siz - 1) // last chunk
            msg += "m=0";

        // Image data
        msg += ";";
        msg += payload[i];
        msg += "\x1b\\";
        if(is_in_tmux())
            msg = tmux_wrap(msg);
        q += msg;
    }

    return q;
}

uint32_t KittyGraphics::send_image(const std::vector<uint8_t>& png, uint32_t id)
{
    // Base64 encoding of the png image
    auto* data = png.data();
    auto size = png.size();
    auto enc = base64::encode(data, size);
    // Chunk it into smaller parts
    auto payload = base64::chunk(enc);

    if(id == 0)
        id = generate_unique_id();

    // Find number of cols and rows necessary to display the image
    auto read32 = [&](size_t i) { return uint32_t(png[i] << 24 | png[i + 1] << 16 | png[i + 2] << 8 | png[i + 3]); };
    auto png_width = read32(16);
    auto png_height = read32(20);

    auto rows = static_cast<unsigned int>(png_height / block_height());
    auto cols = static_cast<unsigned int>(png_width / block_width());

    // Send the image data
    std::string q{send_image_query(payload, id, cols, rows)};
    auto resp = query(q);

    // Store image data for display later
    int col = 0, row = 0;
    if(auto it = _images.find(id); it != end(_images)) // if id exists, reuse previous coordinates
    {
        col = it->second.col;
        row = it->second.row;
    }

    _images[id] = {
        .id = id, .width = png_width, .height = png_height, .cols = cols, .rows = rows, .col = col, .row = row};

    return id;
}

bool KittyGraphics::display_again(uint32_t id)
{
    if(auto it = _images.find(id); it != end(_images))
    {
        auto& image = it->second;
        std::string q{std::format("\x1b_Ga=p,i={},U=1,c={},r={}\x1b\\", id, image.cols, image.rows)};
        if(is_in_tmux())
            q = tmux_wrap(q);
        auto resp = query(q);
        return resp and resp->contains("OK");
    }

    return false;
}

bool KittyGraphics::display_at_cursor(uint32_t id, int indent)
{
    if(auto it = _images.find(id); it != end(_images))
    {
        auto& image = it->second;
        if(auto cp = cursor_position(); cp)
        {
            const auto [row, col] = *cp;
            image.row = row;
            image.col = col + indent;

            std::string q{std::format("\x1b_Ga=p,i={},U=1,c={},r={}\x1b\\", id, image.cols, image.rows)};
            if(is_in_tmux())
                q = tmux_wrap(q);
            q += unicode_placeholders(id, indent, image.rows, image.cols);
            auto resp = query(q);
            return resp and resp->contains("OK");
        }
    }

    return false;
}

bool KittyGraphics::display_at_position(uint32_t id, int row, int col)
{
    if(auto cp = cursor_position(); cp)
    {
        auto [row0, col0] = *cp;
        move_cursor(row, col);
        auto result = display_at_cursor(id, col);
        move_cursor(row0, col0);

        return result;
    }

    return false;
}

void KittyGraphics::delete_image(uint32_t id)
{
    std::string q = std::format("\x1b_Ga=d,d=I,i={}\x1b\\", id);
    if(is_in_tmux())
        q = tmux_wrap(q);
    query(q);
    _images.erase(id);
}

std::string KittyGraphics::diacritic(int value)
{
    std::vector<std::string> encoding = {
        "\u0305",  "\u030D",  "\u030E",  "\u0310",  "\u0312",  "\u033D",  "\u033E",  "\u033F",  "\u0346",  "\u034A",
        "\u034B",  "\u034C",  "\u0350",  "\u0351",  "\u0352",  "\u0357",  "\u035B",  "\u0363",  "\u0364",  "\u0365",
        "\u0366",  "\u0367",  "\u0368",  "\u0369",  "\u036A",  "\u036B",  "\u036C",  "\u036D",  "\u036E",  "\u036F",
        "\u0483",  "\u0484",  "\u0485",  "\u0486",  "\u0487",  "\u0592",  "\u0593",  "\u0594",  "\u0595",  "\u0597",
        "\u0598",  "\u0599",  "\u059C",  "\u059D",  "\u059E",  "\u059F",  "\u05A0",  "\u05A1",  "\u05A8",  "\u05A9",
        "\u05AB",  "\u05AC",  "\u05AF",  "\u05C4",  "\u0610",  "\u0611",  "\u0612",  "\u0613",  "\u0614",  "\u0615",
        "\u0616",  "\u0617",  "\u0657",  "\u0658",  "\u0659",  "\u065A",  "\u065B",  "\u065D",  "\u065E",  "\u06D6",
        "\u06D7",  "\u06D8",  "\u06D9",  "\u06DA",  "\u06DB",  "\u06DC",  "\u06DF",  "\u06E0",  "\u06E1",  "\u06E2",
        "\u06E4",  "\u06E7",  "\u06E8",  "\u06EB",  "\u06EC",  "\u0730",  "\u0732",  "\u0733",  "\u0735",  "\u0736",
        "\u073A",  "\u073D",  "\u073F",  "\u0740",  "\u0741",  "\u0743",  "\u0745",  "\u0747",  "\u0749",  "\u074A",
        "\u07EB",  "\u07EC",  "\u07ED",  "\u07EE",  "\u07EF",  "\u07F0",  "\u07F1",  "\u07F3",  "\u0816",  "\u0817",
        "\u0818",  "\u0819",  "\u081B",  "\u081C",  "\u081D",  "\u081E",  "\u081F",  "\u0820",  "\u0821",  "\u0822",
        "\u0823",  "\u0825",  "\u0826",  "\u0827",  "\u0829",  "\u082A",  "\u082B",  "\u082C",  "\u082D",  "\u0951",
        "\u0953",  "\u0954",  "\u0F82",  "\u0F83",  "\u0F86",  "\u0F87",  "\u135D",  "\u135E",  "\u135F",  "\u17DD",
        "\u193A",  "\u1A17",  "\u1A75",  "\u1A76",  "\u1A77",  "\u1A78",  "\u1A79",  "\u1A7A",  "\u1A7B",  "\u1A7C",
        "\u1B6B",  "\u1B6D",  "\u1B6E",  "\u1B6F",  "\u1B70",  "\u1B71",  "\u1B72",  "\u1B73",  "\u1CD0",  "\u1CD1",
        "\u1CD2",  "\u1CDA",  "\u1CDB",  "\u1CE0",  "\u1DC0",  "\u1DC1",  "\u1DC3",  "\u1DC4",  "\u1DC5",  "\u1DC6",
        "\u1DC7",  "\u1DC8",  "\u1DC9",  "\u1DCB",  "\u1DCC",  "\u1DD1",  "\u1DD2",  "\u1DD3",  "\u1DD4",  "\u1DD5",
        "\u1DD6",  "\u1DD7",  "\u1DD8",  "\u1DD9",  "\u1DDA",  "\u1DDB",  "\u1DDC",  "\u1DDD",  "\u1DDE",  "\u1DDF",
        "\u1DE0",  "\u1DE1",  "\u1DE2",  "\u1DE3",  "\u1DE4",  "\u1DE5",  "\u1DE6",  "\u1DFE",  "\u20D0",  "\u20D1",
        "\u20D4",  "\u20D5",  "\u20D6",  "\u20D7",  "\u20DB",  "\u20DC",  "\u20E1",  "\u20E7",  "\u20E9",  "\u20F0",
        "\u2CEF",  "\u2CF0",  "\u2CF1",  "\u2DE0",  "\u2DE1",  "\u2DE2",  "\u2DE3",  "\u2DE4",  "\u2DE5",  "\u2DE6",
        "\u2DE7",  "\u2DE8",  "\u2DE9",  "\u2DEA",  "\u2DEB",  "\u2DEC",  "\u2DED",  "\u2DEE",  "\u2DEF",  "\u2DF0",
        "\u2DF1",  "\u2DF2",  "\u2DF3",  "\u2DF4",  "\u2DF5",  "\u2DF6",  "\u2DF7",  "\u2DF8",  "\u2DF9",  "\u2DFA",
        "\u2DFB",  "\u2DFC",  "\u2DFD",  "\u2DFE",  "\u2DFF",  "\uA66F",  "\uA67C",  "\uA67D",  "\uA6F0",  "\uA6F1",
        "\uA8E0",  "\uA8E1",  "\uA8E2",  "\uA8E3",  "\uA8E4",  "\uA8E5",  "\uA8E6",  "\uA8E7",  "\uA8E8",  "\uA8E9",
        "\uA8EA",  "\uA8EB",  "\uA8EC",  "\uA8ED",  "\uA8EE",  "\uA8EF",  "\uA8F0",  "\uA8F1",  "\uAAB0",  "\uAAB2",
        "\uAAB3",  "\uAAB7",  "\uAAB8",  "\uAABE",  "\uAABF",  "\uAAC1",  "\uFE20",  "\uFE21",  "\uFE22",  "\uFE23",
        "\uFE24",  "\uFE25",  "\uFE26",  "\u10A0F", "\u10A38", "\u1D185", "\u1D186", "\u1D187", "\u1D188", "\u1D189",
        "\u1D1AA", "\u1D1AB", "\u1D1AC", "\u1D1AD", "\u1D242", "\u1D243", "\u1D244",
    }; /* 297 */

    if(value < 0 || value >= 297)
        return "";
    else
        return encoding[value];
}

std::string KittyGraphics::xy_msb(int x, int y, uint8_t msg)
{
    return diacritic(x) + diacritic(y) + (msg ? diacritic(msg) : "");
}

std::string KittyGraphics::unicode_placeholders(uint32_t id, int indent, int rows, int cols)
{
#define SCREEN_CURSOR_RIGHT_FORMAT "\033[{}C"    // Move cursor right given cols
    std::string placeholder("\xf4\x8e\xbb\xae"); // \u10EEEE
    std::string msg = "\r";
    for(int r = 0; r < rows; r++)
    {
        if(indent > 0)
            msg += std::format(SCREEN_CURSOR_RIGHT_FORMAT, indent);

        // Set foreground color to encode the image ID
        msg += std::format("\033[38:2:{}:{}:{}m",
                           (id >> 16) & 0xff, // Red component
                           (id >> 8) & 0xff,  // Green component
                           id & 0xff);        // Blue component
        for(int c = 0; c < cols; c++)
            msg += placeholder + xy_msb(r, c, (id >> 24) & 0xff);
        msg += "\033[39m\n";
    }

    return msg;
}

// --------------------------------
// SixelGraphics implementation
// --------------------------------

std::optional<SixelGraphics::RawImage> SixelGraphics::decode_png(const std::vector<uint8_t>& png)
{
    int w = 0, h = 0, channels = 0;

    // stbi_load_from_memory always outputs 4 bytes per pixel when req_comp==4
    stbi_uc* data = stbi_load_from_memory(png.data(), static_cast<int>(png.size()), &w, &h, &channels, 4 /*force RGBA*/);

    if(data)
    {
        RawImage img;
        img.width = static_cast<uint32_t>(w);
        img.height = static_cast<uint32_t>(h);
        img.pixels.resize(img.width * img.height);

        for(size_t i = 0; i < img.pixels.size(); ++i)
        {
            img.pixels[i] = {
                data[i * 4 + 0], // R
                data[i * 4 + 1], // G
                data[i * 4 + 2], // B
                data[i * 4 + 3]  // A
            };
        }

        stbi_image_free(data);
        return img;
    }
    else
        return std::nullopt;
}

std::string SixelGraphics::build_sixel(const RawImage& img)
{
    if(img.pixels.empty())
        return {};

    const Palette palette = quantize_colors(img);
    if(palette.empty())
        return {};

    const uint32_t W = img.width;
    const uint32_t H = img.height;
    const int P = static_cast<int>(palette.size());

    // Map every pixel to its nearest palette index
    // -1 is used for transparent pixels (alpha < 128).
    std::vector<int> index_map(W * H, -1);
    for(uint32_t y = 0; y < H; ++y)
        for(uint32_t x = 0; x < W; ++x)
        {
            const auto& pix = img.pixels[y * W + x];
            if(pix.a >= 128)
                index_map[y * W + x] = nearest_color({pix.r, pix.g, pix.b}, palette);
        }

    // Build the sixel stream
    std::string out;
    out.reserve(W * H / 4); // rough lower-bound estimate

    // DCS header - aspect ratio 1:1, background unchanged
    out += "\x1bPq";

    // Colour register definitions
    // #n;2;R;G;B  where R,G,B are percentages (0-100)
    for(int i = 0; i < P; ++i)
    {
        const auto& c = palette[i];
        out += std::format("#{};2;{};{};{}", i, static_cast<int>(c[0] * 100 / 255), static_cast<int>(c[1] * 100 / 255),
                           static_cast<int>(c[2] * 100 / 255));
    }

    // Sixel bands (each band is 6 pixel rows tall)
    const uint32_t num_bands = (H + 5) / 6;

    for(uint32_t band = 0; band < num_bands; ++band)
    {
        const uint32_t y0 = band * 6;
        bool first_in_band = true;

        for(int ci = 0; ci < P; ++ci)
        {
            // Fast check: does colour ci appear anywhere in this band?
            bool used = false;
            for(uint32_t x = 0; x < W && !used; ++x)
                for(int bit = 0; bit < 6 && !used; ++bit)
                {
                    const uint32_t y = y0 + static_cast<uint32_t>(bit);
                    if(y < H && index_map[y * W + x] == ci)
                        used = true;
                }
            if(!used)
                continue;

            // Graphics Carriage Return: return to the start of the current
            // band so the next colour layer is drawn on top.
            if(!first_in_band)
                out += '$';

            first_in_band = false;

            // Select colour register
            out += std::format("#{}", ci);

            // Emit sixel characters for each column, with RLE compression
            char last_ch = '\0';
            int run_len = 0;

            for(uint32_t x = 0; x < W; ++x)
            {
                uint8_t bits = 0;
                for(int bit = 0; bit < 6; ++bit)
                {
                    const uint32_t y = y0 + static_cast<uint32_t>(bit);
                    if(y < H && index_map[y * W + x] == ci)
                        bits |= static_cast<uint8_t>(1 << bit);
                }

                const char ch = static_cast<char>(bits + 0x3f);
                if(ch == last_ch)
                {
                    ++run_len;
                }
                else
                {
                    append_rle(out, last_ch, run_len);
                    last_ch = ch;
                    run_len = 1;
                }
            }
            append_rle(out, last_ch, run_len);
        }

        // Graphics New Line: advance to the next band (skip if last band)
        if(band + 1 < num_bands)
            out += '-';
    }

    // String Terminator - end the DCS sequence
    out += "\x1b\\";

    return out;
}

/// Quantise the image to at most @p max_colors palette entries using a
/// median-cut algorithm on the unique opaque colours in the image.
///
/// Algorithm outline
/// -----------------
///   1. Build a frequency map of all colours with alpha ≥ 128.
///   2. If there are already ≤ max_colors unique colours, return them as-is.
///   3. Otherwise run iterative median-cut: repeatedly split the largest bucket
///      (by colour count) until we reach max_colors buckets.
///   4. Replace each bucket with its average colour.
///
/// Complexity: O(U log U + K·U) where U = unique colour count, K = max_colors.
SixelGraphics::Palette SixelGraphics::quantize_colors(const RawImage& img, int max_colors)
{
    // Hard cap imposed by the sixel specification
    max_colors = std::clamp(max_colors, 1, 256);

    // 1. Frequency map
    std::map<Color3, uint32_t> freq;
    for(const auto& p : img.pixels)
        if(p.a >= 128) // treat low-alpha pixels as transparent / background
            ++freq[{p.r, p.g, p.b}];

    if(freq.empty())
        return {};

    // 2. Collect unique colours
    std::vector<Color3> unique_colors;
    unique_colors.reserve(freq.size());
    for(const auto& [c, _] : freq)
        unique_colors.push_back(c);

    if(static_cast<int>(unique_colors.size()) <= max_colors)
        return unique_colors; // already within budget – done

    // 3. Median-cut
    std::vector<Bucket> buckets(1);
    buckets[0].colors = std::move(unique_colors);

    while(static_cast<int>(buckets.size()) < max_colors)
    {
        // Pick the bucket with the most colours to split
        auto it = std::ranges::max_element(buckets, {}, [](const Bucket& b) { return b.colors.size(); });

        if(it->colors.size() < 2)
            break; // nothing left to split

        auto [lo, hi] = it->split();
        *it = std::move(lo);
        buckets.push_back(std::move(hi));
    }

    // 4. One representative colour per bucket
    Palette palette;
    palette.reserve(buckets.size());
    for(const auto& b : buckets)
        palette.push_back(b.average());

    return palette;
}

void SixelGraphics::append_rle(std::string& out, char ch, int count)
{
    if(count == 0)
        return;
    if(count == 1)
    {
        out += ch;
        return;
    }
    out += std::format("!{}{}", count, ch);
}

int SixelGraphics::nearest_color(const Color3& c, std::span<const Color3> palette)
{
    int best = 0;
    int best_dist = std::numeric_limits<int>::max();
    for(int i = 0; i < static_cast<int>(palette.size()); i++)
    {
        const int dr = static_cast<int>(c[0]) - palette[i][0];
        const int dg = static_cast<int>(c[1]) - palette[i][1];
        const int db = static_cast<int>(c[2]) - palette[i][2];
        const int d = dr * dr + dg * dg + db * db;
        if(d < best_dist)
        {
            best_dist = d;
            best = i;
            if(d == 0)
                break;
        }
    }
    return best;
}

bool SixelGraphics::emit_sixel(uint32_t id, int indent)
{
    const auto sit = _sixel_cache.find(id);
    if(sit == _sixel_cache.end())
        return false;

    // Indent via cursor-right escape (CSI n C)
    if(indent > 0)
        std::print("\033[{}C", indent);

    const std::string& data = sit->second;
    const ssize_t written = write(STDOUT_FILENO, data.data(), static_cast<ssize_t>(data.size()));
    std::cout.flush();

    return written == static_cast<ssize_t>(data.size());
}


/// Build the complete DCS … ST sixel escape sequence for @p img.
///
/// Sixel stream structure
/// ──────────────────────
///   \x1bPq               – DCS + `q` (enter sixel mode)
///   #n;2;R;G;B           – define colour register n  (R,G,B in 0-100 range)
///   … (one per palette entry)
///   [per band]
///     #n <sixel chars>   – switch to colour n, then emit W sixel chars
///     $                  – Graphics Carriage Return (back to column 0)
///     … (repeat for every colour used in this band)
///     -                  – Graphics New Line (advance 6 pixel rows)
///   \x1b\\               – ST (String Terminator – ends the DCS)
///
/// Each sixel character encodes a 1×6 column of pixels as a single byte
/// (bit 0 = top row … bit 5 = bottom row).  The on-screen value is
/// `sixel_bits | 0x3f` (offset so the result is always a printable ASCII char
/// in the range '?' (0x3f, all-off) … '~' (0x7e, all-on)).
///
/// RLE: runs of the same sixel character are compressed as `!<count><char>`.

uint32_t SixelGraphics::send_image(const std::vector<uint8_t>& png, uint32_t id)
{
    const auto raw = decode_png(png);

    if(id == 0)
        id = generate_unique_id();

    _sixel_cache[id] = build_sixel(*raw);

    // Read PNG dimensions from the IHDR chunk (offsets 16-23)
    auto read32 = [&](size_t i) noexcept -> uint32_t {
        return uint32_t(png[i] << 24 | png[i + 1] << 16 | png[i + 2] << 8 | png[i + 3]);
    };
    const uint32_t png_width = read32(16);
    const uint32_t png_height = read32(20);

    const uint32_t img_cols = png_width / block_width();
    const uint32_t img_rows = png_height / block_height();

    // Preserve previously recorded display coordinates if the id already exists
    int col = 0, row = 0;
    if(auto it = _images.find(id); it != _images.end())
    {
        col = it->second.col;
        row = it->second.row;
    }

    _images[id] = {
        .id = id, .width = png_width, .height = png_height, .cols = img_cols, .rows = img_rows, .col = col, .row = row};
    return id;
}

bool SixelGraphics::display_at_cursor(uint32_t id, int indent)
{
    const auto it = _images.find(id);
    if(it == _images.end())
        return false;

    // Record where we displayed the image
    if(const auto cp = cursor_position(); cp)
    {
        const auto [row, col] = *cp;
        it->second.row = row;
        it->second.col = col + indent;
    }

    return emit_sixel(id, indent);
}

bool SixelGraphics::display_at_position(uint32_t id, int row, int col)
{
    // Update stored coordinates
    if(const auto it = _images.find(id); it != _images.end())
    {
        it->second.row = row;
        it->second.col = col;
    }

    const auto cp = cursor_position();
    if(!cp)
        return false;

    const auto [row0, col0] = *cp;
    move_cursor(row, col);
    const bool ok = emit_sixel(id, 0);
    move_cursor(row0, col0);
    return ok;
}

bool SixelGraphics::display_again(uint32_t id)
{
    const auto it = _images.find(id);
    if(it == _images.end())
        return false;

    const auto& img = it->second;

    // If a display position was recorded, re-use it
    if(img.row > 0 || img.col > 0)
        return display_at_position(id, img.row, img.col);

    // Otherwise just emit at the current cursor position
    return emit_sixel(id, 0);
}

void SixelGraphics::delete_image(uint32_t id)
{
    _sixel_cache.erase(id);
    _images.erase(id);
}

// ── iTerm2 protocol implementation ──────────────────────────────────────────────────────────

// ─────────────────────────────────────────────────────────────────────────────
//  ITermGraphics – static helpers
// ─────────────────────────────────────────────────────────────────────────────

/// Build the complete OSC 1337 escape sequence.
///
/// Protocol reference
/// ──────────────────
///   ESC ] 1337 ; File=<params> : <base64-data> ST
///
/// Parameters used
/// ───────────────
///   inline=1               – display the image inline (not as a download)
///   width=<N>              – desired display width in terminal character cells
///   height=<N>             – desired display height in terminal character cells
///   size=<bytes>           – byte length of the original (pre-base64) file;
///                            lets the terminal show a progress bar for large
///                            images and skip malformed payloads cheaply
///   preserveAspectRatio=0  – honour the explicit width/height we computed;
///                            if set to 1 the terminal may override our sizing
///
/// Width / height unit
/// ───────────────────
///   Plain integers are interpreted as character-cell counts by iTerm2 and
///   all compatible terminals.  Pixel counts would require the `px` suffix
///   (e.g. `width=800px`), but character-cell counts map cleanly to the
///   block_width/block_height values already used by KittyGraphics and
///   SixelGraphics, so we use those here too.
///
/// Terminator
/// ──────────
///   We always use ST (\033\\) rather than BEL (\a).  BEL is supported by
///   iTerm2 in a direct connection, but when the sequence is forwarded through
///   tmux_wrap() the helper doubles every ESC byte.  An embedded BEL (\a)
///   cannot be doubled and would be swallowed by tmux, breaking the sequence.
///   ST is safe in all contexts and is the recommended terminator for
///   machine-generated sequences.
[[nodiscard]] std::string ITermGraphics::build_osc1337(const std::string& b64, size_t byte_size, uint32_t img_cols,
                                                       uint32_t img_rows) noexcept
{
    // OSC  1337 ; File = inline=1 ; width=W ; height=H ; size=N ; preserveAspectRatio=0 : <b64> ST
    return std::format("\033]1337;File=inline=1;width={};height={};size={};preserveAspectRatio=0:{}\033\\", img_cols,
                       img_rows, byte_size, b64);
}

/// Emit the cached OSC 1337 sequence to stdout.
///
/// Implementation notes
/// ────────────────────
///   • If @p indent > 0 a CSI cursor-right escape (ESC [ n C) is written
///     first so the image appears indented from the left margin.  OSC 1337
///     itself has no built-in column-offset parameter.
///
///   • When running inside tmux the sequence is forwarded through the DCS
///     passthrough wrapper produced by the inherited tmux_wrap() helper.
///     This requires `set -g allow-passthrough on` in tmux.conf (tmux ≥ 3.3).
///
///   • The write is performed via write(2) rather than through a C++ stream
///     to avoid any buffering interleave with the cursor-positioning escapes
///     emitted by move_cursor().
///
///   • Return value: true iff write(2) reported that all bytes were written.
///     The terminal gives no further confirmation (see class-level note 3).
bool ITermGraphics::emit_iterm(uint32_t id, int indent)
{
    const auto it = _iterm_cache.find(id);
    if(it == _iterm_cache.end())
        return false;

    // Cursor-right indent (CSI n C)
    if(indent > 0)
        std::print("\033[{}C", indent);

    const std::string seq = is_in_tmux() ? tmux_wrap(it->second) : it->second;

    const ssize_t written = write(STDOUT_FILENO, seq.data(), static_cast<ssize_t>(seq.size()));
    std::cout.flush();
    return written == static_cast<ssize_t>(seq.size());
}


// ─────────────────────────────────────────────────────────────────────────────
//  ITermGraphics – public interface
// ─────────────────────────────────────────────────────────────────────────────

/// Base64-encode @p png and cache the complete OSC 1337 escape sequence.
///
/// Unlike Kitty (which stores pixels in the terminal process) and Sixel (which
/// encodes pixels as sixel bands), iTerm2 accepts the original PNG file
/// verbatim — the terminal's own image decoder handles everything.  Client-side
/// work is therefore minimal:
///
///   • Read width & height from the PNG IHDR chunk (bytes 16-23), exactly as
///     KittyGraphics does.  No stb_image or any other decoding is needed.
///   • Compute the character-cell dimensions from block_width()/block_height().
///   • Base64-encode the raw PNG bytes using the existing base64::encode().
///   • Build and cache the OSC 1337 sequence via build_osc1337().
///
/// Return value: the image id (generated when @p id == 0).
/// Returns 0 only if @p png is too small to contain a valid IHDR (< 24 bytes).
uint32_t ITermGraphics::send_image(const std::vector<uint8_t>& png, uint32_t id)
{
    // Sanity: a valid PNG must be at least 24 bytes to contain the IHDR chunk
    constexpr size_t k_ihdr_width_offset = 16;
    constexpr size_t k_ihdr_height_offset = 20;
    constexpr size_t k_min_png_size = 24;

    if(png.size() < k_min_png_size)
        return 0;

    if(id == 0)
        id = generate_unique_id();

    // ── Read PNG dimensions from the IHDR chunk ────────────────────────────
    auto read32 = [&](size_t i) noexcept -> uint32_t {
        return uint32_t(png[i] << 24 | png[i + 1] << 16 | png[i + 2] << 8 | png[i + 3]);
    };
    const uint32_t png_width = read32(k_ihdr_width_offset);
    const uint32_t png_height = read32(k_ihdr_height_offset);

    // Guard against zero block dimensions (e.g. terminal query failed)
    const uint32_t bw = block_width() > 0 ? block_width() : 8;
    const uint32_t bh = block_height() > 0 ? block_height() : 16;

    const uint32_t img_cols = std::max(1u, png_width / bw);
    const uint32_t img_rows = std::max(1u, png_height / bh);

    // ── Base64-encode the raw PNG bytes ────────────────────────────────────
    // We encode the entire PNG file as-is; the terminal decodes it natively.
    // base64::encode() is the existing helper already used by KittyGraphics.
    const std::string b64 = base64::encode(png.data(), png.size());

    // ── Build and cache the OSC 1337 sequence ─────────────────────────────
    _iterm_cache[id] = build_osc1337(b64, png.size(), img_cols, img_rows);

    // ── Store Image metadata ───────────────────────────────────────────────
    // Preserve the display coordinates if the id was already registered
    // (e.g. the caller is refreshing an existing image with new pixel data).
    int col = 0, row = 0;
    if(const auto it = _images.find(id); it != _images.end())
    {
        col = it->second.col;
        row = it->second.row;
    }

    _images[id] = {
        .id = id, .width = png_width, .height = png_height, .cols = img_cols, .rows = img_rows, .col = col, .row = row};
    return id;
}

/// Emit the cached OSC 1337 sequence at the current cursor position.
///
/// The display coordinates are recorded in the Image metadata so that
/// display_again() can re-emit the sequence at the same location later.
///
/// Returns true when write(2) succeeded.  There is no further confirmation
/// from the terminal (see class-level limitation 3).
bool ITermGraphics::display_at_cursor(uint32_t id, int indent)
{
    const auto it = _images.find(id);
    if(it == _images.end())
        return false;

    // Record display coordinates for display_again()
    if(const auto cp = cursor_position(); cp)
    {
        const auto [row, col] = *cp;
        it->second.row = row;
        it->second.col = col + indent;
    }

    return emit_iterm(id, indent);
}

/// Save the cursor, move to (@p row, @p col), emit, then restore the cursor.
///
/// After an OSC 1337 image sequence the terminal advances the cursor to the
/// line below the image.  By saving and restoring explicitly we leave the
/// caller's cursor position unchanged, matching the contract of
/// KittyGraphics::display_at_position().
bool ITermGraphics::display_at_position(uint32_t id, int row, int col)
{
    // Update stored coordinates before emitting
    if(const auto it = _images.find(id); it != _images.end())
    {
        it->second.row = row;
        it->second.col = col;
    }

    const auto cp = cursor_position();
    if(!cp)
        return false;

    const auto [row0, col0] = *cp;
    move_cursor(row, col);
    const bool ok = emit_iterm(id, 0);
    move_cursor(row0, col0);
    return ok;
}

/// Re-emit the cached OSC 1337 sequence at the last recorded display position.
///
/// Kitty achieves this cheaply by sending a short `\x1b_Ga=p,…\x1b\\`
/// reference command that instructs the terminal to re-use its stored pixel
/// data.  iTerm2 has no equivalent command: the only way to redisplay an
/// image is to transmit the full base64 payload again.  The cached sequence in
/// _iterm_cache makes this straightforward, at the cost of retransmitting the
/// full base64 data.
bool ITermGraphics::display_again(uint32_t id)
{
    const auto it = _images.find(id);
    if(it == _images.end())
        return false;

    const auto& img = it->second;

    // Re-use the last recorded display position when available
    if(img.row > 0 || img.col > 0)
        return display_at_position(id, img.row, img.col);

    // Otherwise emit at the current cursor position (indent = 0)
    return emit_iterm(id, 0);
}

/// Free all local resources for @p id.
///
/// LIMITATION – OSC 1337 provides no erase command.
/// The iTerm2 protocol has no escape sequence to remove previously rendered
/// pixels from the terminal's framebuffer.  Overwriting with spaces only
/// covers the character cells, not the underlying graphical layer.  If you
/// need to blank the region, issue ANSI erase-line / erase-display sequences
/// targeting the rows occupied by the image before calling delete_image().
void ITermGraphics::delete_image(uint32_t id)
{
    _iterm_cache.erase(id);
    _images.erase(id);
}
