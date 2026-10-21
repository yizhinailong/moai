/**
 * @file pagination.cppm
 * @brief Cursor-based list pages returned by list endpoints.
 */
export module moai:pagination;

import std;

export namespace moai {

    /**
     * @brief One page of a cursor-paginated list.
     * @tparam T Item type.
     * @note Request the next page by passing `last_id` as the `after` parameter.
     */
    template <typename T>
    struct CursorPage {
        std::vector<T> data{};                 ///< Items in this page.
        std::optional<std::string> first_id{}; ///< ID of the first item, if any.
        std::optional<std::string> last_id{};  ///< ID of the last item, if any.
        bool has_more{};                       ///< Whether more items follow this page.
        std::string object{ "list" };          ///< Object type reported by the API.

        /**
         * @brief Check whether another page can be requested.
         * @return True when the API reports more items and this page has a cursor.
         */
        [[nodiscard]] auto HasNextPage() const noexcept -> bool { return has_more && last_id.has_value() && !data.empty(); }
    };

} // namespace moai
