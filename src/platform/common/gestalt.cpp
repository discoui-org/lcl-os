#include "platform/common/gestalt.hpp"

#include <cmath>
#include <cstdlib>
#include <fstream>
#include <limits>
#include <map>
#include <sstream>
#include <stdexcept>
#include <variant>

namespace lcl::platform {
namespace {

struct JsonValue {
    using Object = std::map<std::string, JsonValue>;
    using Array = std::vector<JsonValue>;
    using Storage = std::variant<std::nullptr_t, bool, double, std::string, Object, Array>;
    Storage value;
};

class JsonParser {
public:
    explicit JsonParser(std::string_view source) : m_source(source) {}

    JsonValue parse() {
        JsonValue root = parseValue(0);
        skipWhitespace();
        if (m_offset != m_source.size()) fail("unexpected trailing content");
        return root;
    }

private:
    [[noreturn]] void fail(const std::string& message) const {
        throw std::runtime_error(message + " at byte " + std::to_string(m_offset));
    }

    void skipWhitespace() {
        while (m_offset < m_source.size()) {
            const char c = m_source[m_offset];
            if (c != ' ' && c != '\t' && c != '\r' && c != '\n') break;
            ++m_offset;
        }
    }

    bool consume(char expected) {
        skipWhitespace();
        if (m_offset >= m_source.size() || m_source[m_offset] != expected) return false;
        ++m_offset;
        return true;
    }

    void expect(char expected) {
        if (!consume(expected)) fail(std::string("expected '") + expected + "'");
    }

    JsonValue parseValue(unsigned depth) {
        if (depth > 32) fail("JSON nesting is too deep");
        skipWhitespace();
        if (m_offset >= m_source.size()) fail("unexpected end of JSON");
        switch (m_source[m_offset]) {
        case '{': return JsonValue{parseObject(depth + 1)};
        case '[': return JsonValue{parseArray(depth + 1)};
        case '"': return JsonValue{parseString()};
        case 't': parseLiteral("true"); return JsonValue{true};
        case 'f': parseLiteral("false"); return JsonValue{false};
        case 'n': parseLiteral("null"); return JsonValue{nullptr};
        default:
            if (m_source[m_offset] == '-' ||
                (m_source[m_offset] >= '0' && m_source[m_offset] <= '9')) {
                return JsonValue{parseNumber()};
            }
            fail("unexpected JSON value");
        }
    }

    JsonValue::Object parseObject(unsigned depth) {
        expect('{');
        JsonValue::Object result;
        if (consume('}')) return result;
        while (true) {
            skipWhitespace();
            if (m_offset >= m_source.size() || m_source[m_offset] != '"') {
                fail("expected object key");
            }
            std::string key = parseString();
            expect(':');
            auto [_, inserted] = result.emplace(std::move(key), parseValue(depth));
            if (!inserted) fail("duplicate object key");
            if (consume('}')) break;
            expect(',');
        }
        return result;
    }

    JsonValue::Array parseArray(unsigned depth) {
        expect('[');
        JsonValue::Array result;
        if (consume(']')) return result;
        while (true) {
            result.push_back(parseValue(depth));
            if (consume(']')) break;
            expect(',');
        }
        return result;
    }

    static void appendUtf8(std::string& output, uint32_t codepoint) {
        if (codepoint <= 0x7f) {
            output.push_back(static_cast<char>(codepoint));
        } else if (codepoint <= 0x7ff) {
            output.push_back(static_cast<char>(0xc0 | (codepoint >> 6)));
            output.push_back(static_cast<char>(0x80 | (codepoint & 0x3f)));
        } else if (codepoint <= 0xffff) {
            output.push_back(static_cast<char>(0xe0 | (codepoint >> 12)));
            output.push_back(static_cast<char>(0x80 | ((codepoint >> 6) & 0x3f)));
            output.push_back(static_cast<char>(0x80 | (codepoint & 0x3f)));
        } else {
            output.push_back(static_cast<char>(0xf0 | (codepoint >> 18)));
            output.push_back(static_cast<char>(0x80 | ((codepoint >> 12) & 0x3f)));
            output.push_back(static_cast<char>(0x80 | ((codepoint >> 6) & 0x3f)));
            output.push_back(static_cast<char>(0x80 | (codepoint & 0x3f)));
        }
    }

    uint32_t parseHexQuad() {
        if (m_source.size() - m_offset < 4) fail("truncated Unicode escape");
        uint32_t value = 0;
        for (int i = 0; i < 4; ++i) {
            const char c = m_source[m_offset++];
            value <<= 4;
            if (c >= '0' && c <= '9') value |= static_cast<uint32_t>(c - '0');
            else if (c >= 'a' && c <= 'f') value |= static_cast<uint32_t>(c - 'a' + 10);
            else if (c >= 'A' && c <= 'F') value |= static_cast<uint32_t>(c - 'A' + 10);
            else fail("invalid Unicode escape");
        }
        return value;
    }

    std::string parseString() {
        expect('"');
        std::string result;
        while (m_offset < m_source.size()) {
            const unsigned char c = static_cast<unsigned char>(m_source[m_offset++]);
            if (c == '"') return result;
            if (c < 0x20) fail("unescaped control character in string");
            if (c != '\\') {
                result.push_back(static_cast<char>(c));
                continue;
            }
            if (m_offset >= m_source.size()) fail("truncated string escape");
            const char escaped = m_source[m_offset++];
            switch (escaped) {
            case '"': result.push_back('"'); break;
            case '\\': result.push_back('\\'); break;
            case '/': result.push_back('/'); break;
            case 'b': result.push_back('\b'); break;
            case 'f': result.push_back('\f'); break;
            case 'n': result.push_back('\n'); break;
            case 'r': result.push_back('\r'); break;
            case 't': result.push_back('\t'); break;
            case 'u': {
                uint32_t codepoint = parseHexQuad();
                if (codepoint >= 0xd800 && codepoint <= 0xdbff) {
                    if (m_source.size() - m_offset < 6 || m_source[m_offset] != '\\' ||
                        m_source[m_offset + 1] != 'u') {
                        fail("missing low Unicode surrogate");
                    }
                    m_offset += 2;
                    const uint32_t low = parseHexQuad();
                    if (low < 0xdc00 || low > 0xdfff) fail("invalid low Unicode surrogate");
                    codepoint = 0x10000 + ((codepoint - 0xd800) << 10) + (low - 0xdc00);
                } else if (codepoint >= 0xdc00 && codepoint <= 0xdfff) {
                    fail("unexpected low Unicode surrogate");
                }
                appendUtf8(result, codepoint);
                break;
            }
            default: fail("invalid string escape");
            }
        }
        fail("unterminated string");
    }

    double parseNumber() {
        const size_t start = m_offset;
        if (m_source[m_offset] == '-') ++m_offset;
        if (m_offset >= m_source.size()) fail("truncated number");
        if (m_source[m_offset] == '0') {
            ++m_offset;
            if (m_offset < m_source.size() && m_source[m_offset] >= '0' &&
                m_source[m_offset] <= '9') {
                fail("leading zero in number");
            }
        } else {
            if (m_source[m_offset] < '1' || m_source[m_offset] > '9') fail("invalid number");
            while (m_offset < m_source.size() && m_source[m_offset] >= '0' &&
                   m_source[m_offset] <= '9') ++m_offset;
        }
        if (m_offset < m_source.size() && m_source[m_offset] == '.') {
            ++m_offset;
            const size_t fractionStart = m_offset;
            while (m_offset < m_source.size() && m_source[m_offset] >= '0' &&
                   m_source[m_offset] <= '9') ++m_offset;
            if (m_offset == fractionStart) fail("missing number fraction");
        }
        if (m_offset < m_source.size() &&
            (m_source[m_offset] == 'e' || m_source[m_offset] == 'E')) {
            ++m_offset;
            if (m_offset < m_source.size() &&
                (m_source[m_offset] == '+' || m_source[m_offset] == '-')) ++m_offset;
            const size_t exponentStart = m_offset;
            while (m_offset < m_source.size() && m_source[m_offset] >= '0' &&
                   m_source[m_offset] <= '9') ++m_offset;
            if (m_offset == exponentStart) fail("missing number exponent");
        }
        const std::string token(m_source.substr(start, m_offset - start));
        char* end = nullptr;
        const double value = std::strtod(token.c_str(), &end);
        if (!end || *end != '\0' || !std::isfinite(value)) fail("invalid finite number");
        return value;
    }

    void parseLiteral(std::string_view literal) {
        if (m_source.substr(m_offset, literal.size()) != literal) fail("invalid literal");
        m_offset += literal.size();
    }

    std::string_view m_source;
    size_t m_offset{0};
};

const JsonValue::Object& requireObject(const JsonValue& value, std::string_view context) {
    const auto* object = std::get_if<JsonValue::Object>(&value.value);
    if (!object) throw std::runtime_error(std::string(context) + " must be an object");
    return *object;
}

const JsonValue::Array& requireArray(const JsonValue& value, std::string_view context) {
    const auto* array = std::get_if<JsonValue::Array>(&value.value);
    if (!array) throw std::runtime_error(std::string(context) + " must be an array");
    return *array;
}

double requireNumber(const JsonValue& value, std::string_view context) {
    const auto* number = std::get_if<double>(&value.value);
    if (!number) throw std::runtime_error(std::string(context) + " must be a number");
    return *number;
}

std::string requireString(const JsonValue& value, std::string_view context) {
    const auto* string = std::get_if<std::string>(&value.value);
    if (!string) throw std::runtime_error(std::string(context) + " must be a string");
    return *string;
}

void rejectUnknownKeys(const JsonValue::Object& object,
                       std::initializer_list<std::string_view> allowed,
                       std::string_view context) {
    for (const auto& [key, _] : object) {
        bool known = false;
        for (const auto candidate : allowed) {
            if (key == candidate) {
                known = true;
                break;
            }
        }
        if (!known) throw std::runtime_error(
            std::string(context) + " contains unknown key '" + key + "'");
    }
}

uint32_t requireUnsigned(const JsonValue& value, std::string_view context,
                         uint32_t minimum, uint32_t maximum) {
    const double number = requireNumber(value, context);
    if (std::floor(number) != number || number < minimum || number > maximum) {
        throw std::runtime_error(
            std::string(context) + " is outside its valid integer range");
    }
    return static_cast<uint32_t>(number);
}

float requireFloat(const JsonValue& value, std::string_view context,
                   float minimum, float maximum) {
    const double number = requireNumber(value, context);
    if (number < minimum || number > maximum) {
        throw std::runtime_error(std::string(context) + " is outside its valid range");
    }
    return static_cast<float>(number);
}

GestaltInsets parseInsets(const JsonValue& value) {
    const auto& object = requireObject(value, "display.safeArea");
    rejectUnknownKeys(object, {"top", "right", "bottom", "left"}, "display.safeArea");
    GestaltInsets result;
    if (auto it = object.find("top"); it != object.end())
        result.top = requireFloat(it->second, "display.safeArea.top", 0.0f, 32768.0f);
    if (auto it = object.find("right"); it != object.end())
        result.right = requireFloat(it->second, "display.safeArea.right", 0.0f, 32768.0f);
    if (auto it = object.find("bottom"); it != object.end())
        result.bottom = requireFloat(it->second, "display.safeArea.bottom", 0.0f, 32768.0f);
    if (auto it = object.find("left"); it != object.end())
        result.left = requireFloat(it->second, "display.safeArea.left", 0.0f, 32768.0f);
    return result;
}

GestaltCorner parseCorner(const JsonValue& value, const std::string& context) {
    const auto& object = requireObject(value, context);
    rejectUnknownKeys(object, {"radiusX", "radiusY", "roundness"}, context);
    GestaltCorner result;
    if (auto it = object.find("radiusX"); it != object.end())
        result.radiusX = requireFloat(it->second, context + ".radiusX", 0.0f, 32768.0f);
    if (auto it = object.find("radiusY"); it != object.end())
        result.radiusY = requireFloat(it->second, context + ".radiusY", 0.0f, 32768.0f);
    if (auto it = object.find("roundness"); it != object.end())
        result.roundness = requireFloat(it->second, context + ".roundness", 2.0f, 8.0f);
    return result;
}

GestaltCorners parseCorners(const JsonValue& value) {
    const auto& object = requireObject(value, "display.corners");
    rejectUnknownKeys(object, {"topLeft", "topRight", "bottomLeft", "bottomRight"},
                      "display.corners");
    GestaltCorners result;
    if (auto it = object.find("topLeft"); it != object.end())
        result.topLeft = parseCorner(it->second, "display.corners.topLeft");
    if (auto it = object.find("topRight"); it != object.end())
        result.topRight = parseCorner(it->second, "display.corners.topRight");
    if (auto it = object.find("bottomLeft"); it != object.end())
        result.bottomLeft = parseCorner(it->second, "display.corners.bottomLeft");
    if (auto it = object.find("bottomRight"); it != object.end())
        result.bottomRight = parseCorner(it->second, "display.corners.bottomRight");
    return result;
}

std::vector<GestaltCutout> parseCutouts(const JsonValue& value) {
    const auto& array = requireArray(value, "display.cutouts");
    if (array.size() > 32) throw std::runtime_error("display.cutouts has too many entries");
    std::vector<GestaltCutout> result;
    result.reserve(array.size());
    for (size_t index = 0; index < array.size(); ++index) {
        const std::string context = "display.cutouts[" + std::to_string(index) + "]";
        const auto& object = requireObject(array[index], context);
        rejectUnknownKeys(object, {"x", "y", "width", "height"}, context);
        GestaltCutout cutout;
        const auto x = object.find("x");
        const auto y = object.find("y");
        const auto width = object.find("width");
        const auto height = object.find("height");
        if (x == object.end() || y == object.end() ||
            width == object.end() || height == object.end()) {
            throw std::runtime_error(context + " requires x, y, width and height");
        }
        cutout.x = requireFloat(x->second, context + ".x", 0.0f, 32768.0f);
        cutout.y = requireFloat(y->second, context + ".y", 0.0f, 32768.0f);
        cutout.width = requireFloat(width->second, context + ".width", 1.0f, 32768.0f);
        cutout.height = requireFloat(height->second, context + ".height", 1.0f, 32768.0f);
        result.push_back(cutout);
    }
    return result;
}

DisplayGestalt parseDisplay(const JsonValue& value) {
    const auto& object = requireObject(value, "display");
    rejectUnknownKeys(object,
                      {"width", "height", "refreshRateHz", "scale",
                       "naturalOrientation", "defaultRotation", "safeArea",
                       "corners", "cutouts"},
                      "display");
    DisplayGestalt result;
    if (auto it = object.find("width"); it != object.end())
        result.width = requireUnsigned(it->second, "display.width", 1, 32768);
    if (auto it = object.find("height"); it != object.end())
        result.height = requireUnsigned(it->second, "display.height", 1, 32768);
    if (result.width.has_value() != result.height.has_value()) {
        throw std::runtime_error("display.width and display.height must be specified together");
    }
    if (auto it = object.find("refreshRateHz"); it != object.end())
        result.refreshRateHz = requireUnsigned(it->second, "display.refreshRateHz", 1, 1000);
    if (auto it = object.find("scale"); it != object.end())
        result.scale = requireFloat(it->second, "display.scale", 0.5f, 4.0f);
    if (auto it = object.find("naturalOrientation"); it != object.end()) {
        const std::string orientation = requireString(it->second, "display.naturalOrientation");
        if (orientation == "auto") result.naturalOrientation = NaturalOrientation::Auto;
        else if (orientation == "portrait") result.naturalOrientation = NaturalOrientation::Portrait;
        else if (orientation == "landscape") result.naturalOrientation = NaturalOrientation::Landscape;
        else throw std::runtime_error(
            "display.naturalOrientation must be auto, portrait or landscape");
    }
    if (auto it = object.find("defaultRotation"); it != object.end()) {
        const uint32_t rotation = requireUnsigned(
            it->second, "display.defaultRotation", 0, 270);
        if (rotation != 0 && rotation != 90 && rotation != 180 && rotation != 270) {
            throw std::runtime_error("display.defaultRotation must be 0, 90, 180 or 270");
        }
        result.defaultRotation = static_cast<uint16_t>(rotation);
    }
    if (auto it = object.find("safeArea"); it != object.end())
        result.safeArea = parseInsets(it->second);
    if (auto it = object.find("corners"); it != object.end())
        result.corners = parseCorners(it->second);
    if (auto it = object.find("cutouts"); it != object.end())
        result.cutouts = parseCutouts(it->second);
    return result;
}

} // namespace

bool parseGestaltJson(std::string_view json, DeviceGestalt& destination,
                      std::string& error) {
    try {
        const JsonValue rootValue = JsonParser(json).parse();
        const auto& root = requireObject(rootValue, "Gestalt root");
        rejectUnknownKeys(root, {"version", "name", "shell", "display"}, "Gestalt root");
        const auto version = root.find("version");
        const auto display = root.find("display");
        if (version == root.end()) throw std::runtime_error("Gestalt version is required");
        if (display == root.end()) throw std::runtime_error("Gestalt display object is required");

        DeviceGestalt parsed;
        parsed.version = requireUnsigned(version->second, "version", 1, 1);
        if (auto name = root.find("name"); name != root.end()) {
            parsed.name = requireString(name->second, "name");
            if (parsed.name.empty() || parsed.name.size() > 256) {
                throw std::runtime_error("name must contain 1 to 256 bytes");
            }
        }
        if (auto shell = root.find("shell"); shell != root.end()) {
            const std::string kind = requireString(shell->second, "shell");
            if (kind == "desktop") parsed.shell = ShellKind::Desktop;
            else if (kind == "mobile") parsed.shell = ShellKind::Mobile;
            else throw std::runtime_error("shell must be desktop or mobile");
        }
        parsed.display = parseDisplay(display->second);
        destination = std::move(parsed);
        error.clear();
        return true;
    } catch (const std::exception& exception) {
        error = exception.what();
        return false;
    }
}

GestaltLoadResult loadGestalt(const std::string& platformDefaultPath) {
    GestaltLoadResult result;
    const char* overridePath = std::getenv("LCL_GESTALT_PATH");
    result.explicitOverride = overridePath && overridePath[0] != '\0';
    result.path = result.explicitOverride ? overridePath : platformDefaultPath;
    if (result.path.empty()) return result;
    if (result.path.front() != '/') {
        result.error = "Gestalt path must be absolute: " + result.path;
        return result;
    }

    std::ifstream file(result.path, std::ios::binary);
    if (!file) {
        if (result.explicitOverride) {
            result.error = "explicit Gestalt file could not be opened: " + result.path;
        }
        return result;
    }
    std::ostringstream contents;
    contents << file.rdbuf();
    const std::string json = contents.str();
    constexpr size_t kMaximumGestaltBytes = 1024 * 1024;
    if (json.size() > kMaximumGestaltBytes) {
        result.error = "Gestalt file exceeds 1 MiB: " + result.path;
        return result;
    }
    if (!parseGestaltJson(json, result.gestalt, result.error)) {
        result.error = "invalid Gestalt '" + result.path + "': " + result.error;
        return result;
    }
    result.loadedFromFile = true;
    return result;
}

} // namespace lcl::platform
