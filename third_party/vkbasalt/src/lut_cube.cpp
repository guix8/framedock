#include "lut_cube.hpp"

#include "logger.hpp"

#include <cerrno>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <locale.h>
#include <sstream>

namespace vkBasalt
{
    namespace
    {
        /** @return text after leading blanks */
        const char* skipBlanks(const char* text)
        {
            while (*text == ' ' || *text == '\t' || *text == '\r')
                ++text;
            return text;
        }

        /**
         * @return strtof in the C locale: "0.5" whatever locale the compositor runs in (in de_DE, plain strtof stops
         *         at the '.')
         */
        float strtofC(const char* text, char** end)
        {
            static const locale_t c = newlocale(LC_ALL_MASK, "C", static_cast<locale_t>(nullptr));
            return c ? strtof_l(text, end, c) : std::strtof(text, end);
        }

        /** @return true if text starts with the keyword followed by a blank (or the end) */
        bool keyword(const char* text, const char* name, const char*& rest)
        {
            const size_t n = std::strlen(name);
            if (std::strncmp(text, name, n) != 0)
                return false;
            const char after = text[n];
            if (after != '\0' && after != ' ' && after != '\t' && after != '\r')
                return false;
            rest = text + n;
            return true;
        }
    } // namespace

    LutCube::LutCube()
    {
    }

    LutCube::LutCube(const std::string& file)
    {
        std::ifstream cubeStream(file);
        if (!cubeStream.good())
        {
            fail("lut cube file can't be read: " + file);
            return;
        }
        std::stringstream text;
        text << cubeStream.rdbuf();
        parse(text.str());
    }

    bool LutCube::parse(const std::string& text)
    {
        size    = 0;
        points_ = 0;
        error_.clear();
        colorCube.clear();
        minX = minY = minZ = 0.0f;
        maxX = maxY = maxZ = 1.0f;
        // A leading UTF-8 byte order mark (some editors on Windows save one) is no part of the first line
        const bool bom = text.size() >= 3 && text.compare(0, 3, "\xEF\xBB\xBF") == 0;
        std::istringstream lines(bom ? text.substr(3) : text);
        std::string        line;
        int                number = 0;
        while (std::getline(lines, line))
        {
            ++number;
            if (!parseLine(line, number))
                return false;
        }
        if (size == 0)
        {
            fail("no LUT_3D_SIZE in the lut cube file");
            return false;
        }
        const size_t expected = static_cast<size_t>(size) * size * size;
        if (points_ != expected)
        {
            fail("the lut cube has " + std::to_string(points_) + " data lines, LUT_3D_SIZE " + std::to_string(size)
                 + " needs " + std::to_string(expected));
            return false;
        }
        return true;
    }

    bool LutCube::parseLine(const std::string& raw, int lineNumber)
    {
        const char* line = skipBlanks(raw.c_str());
        if (*line == '\0' || *line == '#')
            return true;
        const std::string where = " (line " + std::to_string(lineNumber) + ")";
        const char*       rest  = nullptr;
        if (keyword(line, "LUT_3D_SIZE", rest))
        {
            if (size != 0)
            {
                fail("a second LUT_3D_SIZE" + where);
                return false;
            }
            errno       = 0;
            char*      end   = nullptr;
            const long value = std::strtol(rest, &end, 10);
            if (end == rest || *skipBlanks(end) != '\0' || errno != 0 || value < kMinSize || value > kMaxSize)
            {
                fail("LUT_3D_SIZE must be a whole number in " + std::to_string(kMinSize) + ".." + std::to_string(kMaxSize)
                     + where);
                return false;
            }
            size = static_cast<int>(value);
            colorCube.assign(static_cast<size_t>(size) * size * size * 4, 255);
            return true;
        }
        if (keyword(line, "LUT_1D_SIZE", rest))
        {
            fail("1D lut cubes aren't supported" + where);
            return false;
        }
        if (keyword(line, "DOMAIN_MIN", rest) || keyword(line, "DOMAIN_MAX", rest))
        {
            const bool isMin = line[7] == 'M' && line[8] == 'I';
            float      x = 0, y = 0, z = 0;
            if (!splitTripel(rest, x, y, z))
            {
                fail(std::string(isMin ? "DOMAIN_MIN" : "DOMAIN_MAX") + " needs three numbers" + where);
                return false;
            }
            if (isMin)
            {
                minX = x;
                minY = y;
                minZ = z;
            }
            else
            {
                maxX = x;
                maxY = y;
                maxZ = z;
            }
            if (points_ > 0)
            {
                fail("a domain after the data" + where);
                return false;
            }
            return true;
        }
        if (keyword(line, "TITLE", rest))
            return true;
        if ((*line >= '0' && *line <= '9') || *line == '-' || *line == '+' || *line == '.')
        {
            if (size == 0)
            {
                fail("a data line before LUT_3D_SIZE" + where);
                return false;
            }
            const size_t expected = static_cast<size_t>(size) * size * size;
            if (points_ >= expected)
            {
                fail("more data lines than LUT_3D_SIZE " + std::to_string(size) + " allows" + where);
                return false;
            }
            float x = 0, y = 0, z = 0;
            if (!splitTripel(line, x, y, z))
            {
                fail("a data line needs three numbers" + where);
                return false;
            }
            if (!(maxX > minX) || !(maxY > minY) || !(maxZ > minZ))
            {
                fail("an empty or reversed DOMAIN" + where);
                return false;
            }
            writeColor(points_, toByte(x, minX, maxX), toByte(y, minY, maxY), toByte(z, minZ, maxZ));
            ++points_;
            return true;
        }
        // Other keywords of the format (LUT_3D_INPUT_RANGE, ...) are ignored, as before
        return true;
    }

    bool LutCube::splitTripel(const char* text, float& x, float& y, float& z)
    {
        float*      out[3] = {&x, &y, &z};
        const char* p      = text;
        for (float* value : out)
        {
            p           = skipBlanks(p);
            char* end   = nullptr;
            errno       = 0;
            const float v = strtofC(p, &end);
            if (end == p || errno == ERANGE || !std::isfinite(v))
                return false;
            *value = v;
            p      = end;
        }
        return *skipBlanks(p) == '\0';
    }

    unsigned char LutCube::toByte(float value, float lo, float hi)
    {
        const float t = std::fmin(1.0f, std::fmax(0.0f, (value - lo) / (hi - lo)));
        return static_cast<unsigned char>(std::lround(t * 255.0f));
    }

    void LutCube::writeColor(size_t index, unsigned char r, unsigned char g, unsigned char b)
    {
        // The file's order: red fastest, then green, then blue (x, y, z)
        const size_t location = index * 4;
        colorCube[location + 0] = r;
        colorCube[location + 1] = g;
        colorCube[location + 2] = b;
    }

    void LutCube::identity(int cubeSize)
    {
        size = cubeSize;
        colorCube.assign(static_cast<size_t>(size) * size * size * 4, 255);
        for (int blue = 0; blue < size; ++blue)
            for (int green = 0; green < size; ++green)
                for (int red = 0; red < size; ++red)
                {
                    const size_t index = (static_cast<size_t>(blue) * size + green) * size + red;
                    const auto   level = [&](int v) {
                        return static_cast<unsigned char>(std::lround(255.0 * v / (size - 1)));
                    };
                    writeColor(index, level(red), level(green), level(blue));
                }
    }

    void LutCube::fail(const std::string& why)
    {
        error_ = why;
        Logger::err(why + ": using an identity lut cube instead");
        identity(kFallbackSize);
    }
} // namespace vkBasalt
