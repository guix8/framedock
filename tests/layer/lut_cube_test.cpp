// The Vulkan layer's .cube reader (third_party/vkbasalt/src/lut_cube.cpp): it runs inside vrcompositor, so a bad
// file must never throw or abort, only fall back to an identity cube and say why.
#include "logger.hpp"
#include "lut_cube.hpp"

#include <unistd.h>

#include <clocale>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <string>

// The layer defines its one logger in basalt.cpp (not built here)
vkBasalt::Logger vkBasalt::Logger::s_instance;

namespace {

int failures = 0;

#define CHECK(expr)                                             \
    do {                                                        \
        if (!(expr)) {                                          \
            std::printf("FAIL line %d: %s\n", __LINE__, #expr); \
            ++failures;                                         \
        }                                                       \
    } while (false)

using vkBasalt::LutCube;

/** @return true if cube is the identity fallback (size kFallbackSize, corners black and white) */
bool isFallback(const LutCube& cube) {
    const int n = LutCube::kFallbackSize;
    if (cube.ok() || cube.size != n || cube.colorCube.size() != static_cast<size_t>(n) * n * n * 4) return false;
    const size_t last = cube.colorCube.size() - 4;
    return cube.colorCube[0] == 0 && cube.colorCube[1] == 0 && cube.colorCube[2] == 0 && cube.colorCube[3] == 255 &&
           cube.colorCube[last] == 255 && cube.colorCube[last + 1] == 255 && cube.colorCube[last + 2] == 255;
}

bool fails(const std::string& text, const std::string& why) {
    LutCube cube;
    const bool parsed = cube.parse(text);
    if (parsed || !isFallback(cube) || cube.error().find(why) == std::string::npos) {
        std::printf("  for %s: parsed %d, error \"%s\"\n", why.c_str(), parsed, cube.error().c_str());
        return false;
    }
    return true;
}

const std::string kGood2 =
    "# a comment\n"
    "TITLE \"test\"\n"
    "LUT_3D_SIZE 2\n"
    "DOMAIN_MIN 0 0 0\n"
    "DOMAIN_MAX 1 1 1\n"
    "0 0 0\n1 0 0\n0 1 0\n1 1 0\n"
    "0 0 1\n1 0 1\n0 1 1\n0.5 0.25 1\n";

}  // namespace

int main() {
    {
        LutCube cube;
        CHECK(cube.parse(kGood2));
        CHECK(cube.ok() && cube.size == 2 && cube.colorCube.size() == 32);
        CHECK(cube.colorCube[4] == 255 && cube.colorCube[5] == 0 && cube.colorCube[6] == 0 && cube.colorCube[7] == 255);
        CHECK(cube.colorCube[28] == 128 && cube.colorCube[29] == 64 && cube.colorCube[30] == 255);
    }
    {
        // A wider domain scales the values; values outside it are clamped (no wrap-around through a byte cast)
        LutCube cube;
        CHECK(cube.parse("LUT_3D_SIZE 2\nDOMAIN_MIN 0 0 0\nDOMAIN_MAX 2 2 2\n"
                         "2 1 0\n-1 5 0\n0 0 0\n0 0 0\n0 0 0\n0 0 0\n0 0 0\n0 0 0\n"));
        CHECK(cube.colorCube[0] == 255 && cube.colorCube[1] == 128 && cube.colorCube[2] == 0);
        CHECK(cube.colorCube[4] == 0 && cube.colorCube[5] == 255);
    }
    // CRLF line ends and blanks are fine
    {
        LutCube cube;
        CHECK(cube.parse("LUT_3D_SIZE 2\r\n\t0 0 0\r\n1 0 0 \r\n0 1 0\n1 1 0\n0 0 1\n1 0 1\n0 1 1\n1 1 1\n"));
        CHECK(cube.ok());
    }
    // A leading UTF-8 byte order mark is skipped (before a comment or a keyword)
    {
        LutCube cube;
        CHECK(cube.parse("\xEF\xBB\xBF" + kGood2));
        CHECK(cube.ok() && cube.size == 2 && cube.colorCube[28] == 128);
        CHECK(cube.parse("\xEF\xBB\xBFLUT_3D_SIZE 2\n0 0 0\n1 0 0\n0 1 0\n1 1 0\n0 0 1\n1 0 1\n0 1 1\n1 1 1\n"));
        CHECK(cube.ok());
    }
    // Decimal points whatever the process's locale (a comma-decimal locale makes plain strtof stop at the '.'); the
    // check runs only where such a locale is installed
    {
        const char* commaLocales[] = {"de_DE.UTF-8", "de_DE.utf8", "fr_FR.UTF-8", "fr_FR.utf8"};
        bool switched = false;
        for (const char* name : commaLocales) {
            if (std::setlocale(LC_NUMERIC, name)) {
                switched = true;
                break;
            }
        }
        if (switched) {
            CHECK(std::strtof("0.5", nullptr) != 0.5f);  // the locale really reads commas
            LutCube cube;
            CHECK(cube.parse(kGood2));
            CHECK(cube.ok() && cube.colorCube[28] == 128 && cube.colorCube[29] == 64);
            std::setlocale(LC_NUMERIC, "C");
        } else {
            std::printf("lut-cube: no comma-decimal locale installed, the locale check was skipped\n");
        }
    }
    CHECK(fails("", "no LUT_3D_SIZE"));
    CHECK(fails("0 0 0\nLUT_3D_SIZE 2\n", "before LUT_3D_SIZE"));
    CHECK(fails("LUT_3D_SIZE 2\n0 0 0\n", "has 1 data lines"));
    CHECK(fails(kGood2 + "1 1 1\n", "more data lines"));
    CHECK(fails("LUT_3D_SIZE 1\n", "LUT_3D_SIZE must be"));
    CHECK(fails("LUT_3D_SIZE 300\n", "LUT_3D_SIZE must be"));
    CHECK(fails("LUT_3D_SIZE abc\n", "LUT_3D_SIZE must be"));
    CHECK(fails("LUT_3D_SIZE 99999999999999999999\n", "LUT_3D_SIZE must be"));
    CHECK(fails("LUT_3D_SIZE 2x\n", "LUT_3D_SIZE must be"));
    CHECK(fails("LUT_3D_SIZE 2\nLUT_3D_SIZE 3\n", "a second LUT_3D_SIZE"));
    CHECK(fails("LUT_1D_SIZE 16\n", "1D"));
    CHECK(fails("LUT_3D_SIZE 2\n0 x 0\n", "three numbers"));
    CHECK(fails("LUT_3D_SIZE 2\n0 0\n", "three numbers"));
    CHECK(fails("LUT_3D_SIZE 2\n0 0 0 0\n", "three numbers"));
    CHECK(fails("LUT_3D_SIZE 2\n0 nan 0\n", "three numbers"));
    CHECK(fails("LUT_3D_SIZE 2\n0 1e99 0\n", "three numbers"));
    CHECK(fails("LUT_3D_SIZE 2\nDOMAIN_MIN 0 0\n", "DOMAIN_MIN needs"));
    CHECK(fails("LUT_3D_SIZE 2\nDOMAIN_MAX 1 1 x\n", "DOMAIN_MAX needs"));
    CHECK(fails("LUT_3D_SIZE 2\nDOMAIN_MAX 1 0 1\n0 0 0\n", "empty or reversed"));
    // A file that isn't there
    {
        LutCube cube("/nonexistent/framecolor-test.cube");
        CHECK(isFallback(cube) && cube.error().find("can't be read") != std::string::npos);
    }
    // From a real file (the constructor's path)
    {
        char path[] = "/tmp/lut-cube-test-XXXXXX";
        const int fd = ::mkstemp(path);
        CHECK(fd >= 0);
        if (fd >= 0) {
            ::close(fd);
            std::ofstream(path) << kGood2;
            LutCube cube(path);
            CHECK(cube.ok() && cube.size == 2);
            ::unlink(path);
        }
    }
    if (failures == 0) std::printf("lut-cube: all passed\n");
    return failures == 0 ? 0 : 1;
}
