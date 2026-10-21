/**
 * @file json.cppm
 * @brief JSON value type for free-form fields and internal decoding helpers.
 * @note nlohmann::json treats braces as an initializer list: `Json value{ other }` creates a
 * one-element array. Initialize Json objects with `=` or parentheses instead.
 */
export module moai:json;

import std;
import nlohmann.json;

export namespace moai {

    /**
     * @brief Owned JSON value for fields whose shape is defined by the caller or the API,
     * such as JSON Schemas, tool parameters, and extra request body fields.
     */
    using Json = nlohmann::json;

} // namespace moai

namespace moai::detail {

    /**
     * @brief Internal decoding failure carrying the JSON path of the offending value.
     * @note Thrown only inside decoders and converted to ErrorKind::Decode at the API boundary.
     */
    class DecodeFailure : public std::runtime_error {
    public:
        using std::runtime_error::runtime_error;
    };

    /**
     * @brief Read a JSON value while tracking its path for diagnostics.
     * @note Absent members and explicit nulls are both treated as missing; present values of the
     * wrong type throw DecodeFailure. The referenced value must outlive the reader.
     */
    class JsonReader {
    private:
        Json const* m_value; ///< Borrowed value being read.
        std::string m_path;  ///< Dotted path used in diagnostics.

    public:
        /**
         * @brief Borrow a value and record its path.
         * @param value Value to read; must outlive the reader.
         * @param path Path of the value, empty for the document root.
         */
        JsonReader(Json const& value, std::string path = {}) : m_value{ &value }, m_path{ std::move(path) } {}

        /**
         * @brief Return the borrowed value.
         */
        [[nodiscard]] auto Value() const -> Json const& { return *m_value; }

        /**
         * @brief Return the diagnostic path of this value.
         */
        [[nodiscard]] auto Path() const -> std::string const& { return m_path; }

        /**
         * @brief Look up a present, non-null object member.
         * @param key Member name.
         * @return A reader for the member, or nullopt when absent, null, or this is not an object.
         */
        [[nodiscard]] auto Field(std::string_view key) const -> std::optional<JsonReader> {
            if (!m_value->is_object()) {
                return std::nullopt;
            }
            auto const found{ m_value->find(key) };
            if (found == m_value->end() || found->is_null()) {
                return std::nullopt;
            }
            return JsonReader{ *found, m_path.empty() ? std::string{ key } : std::format("{}.{}", m_path, key) };
        }

        /**
         * @brief Require this value to be an object.
         * @throws DecodeFailure If it is another type.
         */
        auto RequireObject() const -> void {
            if (!m_value->is_object()) {
                fail("object");
            }
        }

        /**
         * @brief Read a string.
         * @throws DecodeFailure If the value is not a string.
         */
        [[nodiscard]] auto String() const -> std::string {
            if (!m_value->is_string()) {
                fail("string");
            }
            return m_value->get<std::string>();
        }

        /**
         * @brief Read an integer; floating-point values without a fractional part are accepted.
         * @throws DecodeFailure If the value is not an integral number.
         */
        [[nodiscard]] auto Int() const -> std::int64_t {
            if (m_value->is_number_integer()) {
                return m_value->get<std::int64_t>();
            }
            if (m_value->is_number_float()) {
                auto const number{ m_value->get<double>() };
                if (std::isfinite(number) && std::trunc(number) == number) {
                    return static_cast<std::int64_t>(number);
                }
            }
            fail("integer");
        }

        /**
         * @brief Read a number as double.
         * @throws DecodeFailure If the value is not a number.
         */
        [[nodiscard]] auto Number() const -> double {
            if (!m_value->is_number()) {
                fail("number");
            }
            return m_value->get<double>();
        }

        /**
         * @brief Read a boolean.
         * @throws DecodeFailure If the value is not a boolean.
         */
        [[nodiscard]] auto Bool() const -> bool {
            if (!m_value->is_boolean()) {
                fail("boolean");
            }
            return m_value->get<bool>();
        }

        /**
         * @brief Decode every element of an array.
         * @param decode Callable receiving a JsonReader for each element.
         * @return Decoded elements in order.
         * @throws DecodeFailure If the value is not an array or an element fails to decode.
         */
        template <typename Decode>
        [[nodiscard]] auto Array(Decode&& decode) const -> std::vector<std::invoke_result_t<Decode&, JsonReader const&>> {
            if (!m_value->is_array()) {
                fail("array");
            }
            std::vector<std::invoke_result_t<Decode&, JsonReader const&>> result;
            result.reserve(m_value->size());
            for (std::size_t index{}; index < m_value->size(); ++index) {
                result.push_back(decode(JsonReader{ (*m_value)[index], std::format("{}[{}]", m_path, index) }));
            }
            return result;
        }

        /**
         * @brief Read an optional string member.
         */
        [[nodiscard]] auto OptionalString(std::string_view key) const -> std::optional<std::string> {
            if (auto const field{ Field(key) }) {
                return field->String();
            }
            return std::nullopt;
        }

        /**
         * @brief Read a string member, returning an empty string when missing.
         */
        [[nodiscard]] auto StringOr(std::string_view key) const -> std::string { return OptionalString(key).value_or(std::string{}); }

        /**
         * @brief Read an optional integer member.
         */
        [[nodiscard]] auto OptionalInt(std::string_view key) const -> std::optional<std::int64_t> {
            if (auto const field{ Field(key) }) {
                return field->Int();
            }
            return std::nullopt;
        }

        /**
         * @brief Read an integer member, returning zero when missing.
         */
        [[nodiscard]] auto IntOr(std::string_view key) const -> std::int64_t { return OptionalInt(key).value_or(0); }

        /**
         * @brief Read an optional numeric member.
         */
        [[nodiscard]] auto OptionalNumber(std::string_view key) const -> std::optional<double> {
            if (auto const field{ Field(key) }) {
                return field->Number();
            }
            return std::nullopt;
        }

        /**
         * @brief Read an optional boolean member.
         */
        [[nodiscard]] auto OptionalBool(std::string_view key) const -> std::optional<bool> {
            if (auto const field{ Field(key) }) {
                return field->Bool();
            }
            return std::nullopt;
        }

        /**
         * @brief Decode an array member, returning an empty vector when missing.
         */
        template <typename Decode>
        [[nodiscard]] auto ArrayOr(std::string_view key, Decode&& decode) const -> std::vector<std::invoke_result_t<Decode&, JsonReader const&>> {
            if (auto const field{ Field(key) }) {
                return field->Array(std::forward<Decode>(decode));
            }
            return {};
        }

    private:
        /**
         * @brief Throw a type mismatch for this value.
         * @param expected Expected JSON type name.
         */
        [[noreturn]] auto fail(std::string_view expected) const -> void {
            throw DecodeFailure{ std::format("{}: expected {}, got {}", m_path.empty() ? "<root>" : m_path, expected, m_value->type_name()) };
        }
    };

    /**
     * @brief Store an optional value under a key only when it is set.
     * @param object Destination object.
     * @param key Member name.
     * @param value Optional value convertible to Json.
     */
    template <typename T>
    auto put_optional(Json& object, std::string_view key, std::optional<T> const& value) -> void {
        if (value) {
            object[std::string{ key }] = *value;
        }
    }

} // namespace moai::detail
