#ifndef LUT_CUBE_HPP_INCLUDED
#define LUT_CUBE_HPP_INCLUDED

#include <string>
#include <vector>

namespace vkBasalt
{
    /*
       reads .cube files
       returns a vector of bytes
       one byte is one color value
       4 bytes stand for rgba
       the alpha value is always 255

       size will be set according to the size in the file, which can be in [2,256]
       the cube will have the dimentions size * size * size

       so the vector will have a length of size*size*size*4

       See: https://wwwimages2.adobe.com/content/dam/acom/en/products/speedgrade/cc/pdfs/cube-lut-specification-1.0.pdf

       FrameColor: this runs inside vrcompositor, so nothing here throws or aborts. A file that can't be read or isn't
       a valid 3D cube (no LUT_3D_SIZE, a size out of range, a data line before the size, a wrong number of data
       lines, a value that isn't a number, an empty domain, 1D LUTs) gives an identity cube of kFallbackSize instead,
       with ok() false and error() saying why. The layer's live LUT (current.txt) replaces it anyway.
    */
    class LutCube
    {
    public:
        static constexpr int kMinSize      = 2;
        static constexpr int kMaxSize      = 256;
        static constexpr int kFallbackSize = 33;  ///< FrameColor's own cubes: an identity this size on any error

        std::vector<unsigned char> colorCube;
        int                        size = 0;

        explicit LutCube(const std::string& file);
        LutCube();

        /** Parses the text of a .cube file (the constructor reads the file and calls this). */
        bool parse(const std::string& text);

        bool               ok() const { return error_.empty(); }
        const std::string& error() const { return error_; }

    private:
        float minX = 0.0f;
        float minY = 0.0f;
        float minZ = 0.0f;

        float maxX = 1.0f;
        float maxY = 1.0f;
        float maxZ = 1.0f;

        size_t      points_ = 0;  ///< data lines read so far
        std::string error_;

        /** @return false (with error_ set) if the line is invalid */
        bool parseLine(const std::string& line, int lineNumber);
        /** Three numbers (strtof, each checked); nothing but blanks after them */
        static bool splitTripel(const char* text, float& x, float& y, float& z);
        static unsigned char toByte(float value, float lo, float hi);
        void writeColor(size_t index, unsigned char r, unsigned char g, unsigned char b);
        void identity(int cubeSize);
        void fail(const std::string& why);
    };

} // namespace vkBasalt
#endif // LUT_CUBE_HPP_INCLUDED
