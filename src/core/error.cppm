/**
 * @file error.cppm
 * @brief SDK error classification and the Result alias returned by every request.
 */
export module moai:error;

import std;
import mcr;

export namespace moai {

    /**
     * @brief Origin and category of an SDK error.
     * @note Values from BadRequest through Status describe HTTP responses outside 2xx; their
     * Error objects carry the status code, request ID, and decoded API error fields.
     */
    enum class ErrorKind : std::uint8_t {
        InvalidArgument,     ///< The request could not be built, for example invalid options or serialization failure.
        RequestSetup,        ///< mcr rejected the request configuration before any transfer.
        Connection,          ///< The transfer failed without an HTTP response, such as DNS, TLS, or reset errors.
        Timeout,             ///< The transfer exceeded its timeout.
        Cancelled,           ///< The caller cancelled the request or a stream callback stopped it.
        BadRequest,          ///< HTTP 400.
        Authentication,      ///< HTTP 401.
        PermissionDenied,    ///< HTTP 403.
        NotFound,            ///< HTTP 404.
        Conflict,            ///< HTTP 409.
        UnprocessableEntity, ///< HTTP 422.
        RateLimit,           ///< HTTP 429.
        InternalServer,      ///< HTTP 500 and above.
        Status,              ///< Any other status outside 2xx.
        Decode,              ///< The response body was not the expected JSON shape.
        Stream,              ///< A stream reported an error event, ended early, or was not an event stream.
    };

    /**
     * @brief Return a stable name for an error kind.
     * @param kind Kind to name.
     * @return Static, null-terminated name.
     */
    [[nodiscard]] constexpr auto to_string(ErrorKind kind) noexcept -> std::string_view {
        switch (kind) {
            case ErrorKind::InvalidArgument:
                return "InvalidArgument";
            case ErrorKind::RequestSetup:
                return "RequestSetup";
            case ErrorKind::Connection:
                return "Connection";
            case ErrorKind::Timeout:
                return "Timeout";
            case ErrorKind::Cancelled:
                return "Cancelled";
            case ErrorKind::BadRequest:
                return "BadRequest";
            case ErrorKind::Authentication:
                return "Authentication";
            case ErrorKind::PermissionDenied:
                return "PermissionDenied";
            case ErrorKind::NotFound:
                return "NotFound";
            case ErrorKind::Conflict:
                return "Conflict";
            case ErrorKind::UnprocessableEntity:
                return "UnprocessableEntity";
            case ErrorKind::RateLimit:
                return "RateLimit";
            case ErrorKind::InternalServer:
                return "InternalServer";
            case ErrorKind::Status:
                return "Status";
            case ErrorKind::Decode:
                return "Decode";
            case ErrorKind::Stream:
                return "Stream";
        }
        return "Unknown";
    }

    /**
     * @brief Map an HTTP status outside 2xx to its error kind, following the official SDK classes.
     * @param status_code HTTP status code.
     * @return Matching kind, or ErrorKind::Status for unlisted codes.
     */
    [[nodiscard]] constexpr auto error_kind_from_status(long status_code) noexcept -> ErrorKind {
        switch (status_code) {
            case 400:
                return ErrorKind::BadRequest;
            case 401:
                return ErrorKind::Authentication;
            case 403:
                return ErrorKind::PermissionDenied;
            case 404:
                return ErrorKind::NotFound;
            case 409:
                return ErrorKind::Conflict;
            case 422:
                return ErrorKind::UnprocessableEntity;
            case 429:
                return ErrorKind::RateLimit;
            default:
                return status_code >= 500 ? ErrorKind::InternalServer : ErrorKind::Status;
        }
    }

    /**
     * @brief An owned description of a failed request.
     * @note Fields that do not apply to the kind stay empty; for example status_code is zero
     * for transport errors. API keys are never included.
     */
    struct Error {
        ErrorKind kind{ ErrorKind::InvalidArgument };        ///< Error category.
        std::string message{};                               ///< Human-readable diagnostic.
        long status_code{};                                  ///< HTTP status, or zero without a response.
        std::string request_id{};                            ///< Value of the x-request-id response header, if any.
        std::optional<std::string> type{};                   ///< API error `type`, if returned.
        std::optional<std::string> code{};                   ///< API error `code`, if returned.
        std::optional<std::string> param{};                  ///< API error `param`, if returned.
        std::string body{};                                  ///< Raw error response body, truncated to a bounded size.
        mcr::ErrorCode transport_code{ mcr::ErrorCode::OK }; ///< mcr error code for setup and transport failures.

        /**
         * @brief Check whether this error describes an HTTP response outside 2xx.
         */
        [[nodiscard]] auto IsStatusError() const noexcept -> bool {
            return kind >= ErrorKind::BadRequest && kind <= ErrorKind::Status;
        }
    };

    /**
     * @brief Value or Error returned by moai operations.
     */
    template <typename T>
    using Result = std::expected<T, Error>;

} // namespace moai

/**
 * @brief Format an Error as `Kind (HTTP status, request id): message`.
 */
template <>
struct std::formatter<moai::Error> : std::formatter<std::string_view> {
    /**
     * @brief Write the formatted error.
     * @param error Error to describe.
     * @param context Format context.
     * @return Output iterator after the text.
     */
    auto format(moai::Error const& error, std::format_context& context) const -> std::format_context::iterator {
        std::string text{ moai::to_string(error.kind) };
        if (error.status_code != 0 || !error.request_id.empty()) {
            text += " (";
            if (error.status_code != 0) {
                text += std::format("HTTP {}", error.status_code);
            }
            if (!error.request_id.empty()) {
                text += std::format("{}request {}", error.status_code != 0 ? ", " : "", error.request_id);
            }
            text += ')';
        }
        if (!error.message.empty()) {
            text += ": ";
            text += error.message;
        }
        return std::formatter<std::string_view>::format(text, context);
    }
};
