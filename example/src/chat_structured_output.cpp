/**
 * @file chat_structured_output.cpp
 * @brief Request JSON Schema output and parse it with moai's helper.
 * @note Reads OPENAI_API_KEY and, if set, OPENAI_BASE_URL. Usage: chat_structured_output [model]
 */
import std;
import moai;

/**
 * @brief Run the example.
 * @return Zero on success.
 */
auto main(int argc, char** argv) -> int {
    std::string const model{ argc > 1 ? argv[1] : "gpt-5-mini" };
    auto client{ moai::Client::Create() };
    if (!client) {
        std::println(std::cerr, "{}", client.error());
        return 1;
    }

    auto const completion{ client->chat.completions.Create({
        .model = model,
        .messages = {
            moai::system_message("Extract the person and place mentioned in the text."),
            moai::user_message("Ada Lovelace was born in London in 1815."),
        },
        .response_format = moai::ResponseFormatJsonSchema{
            .json_schema = {
                .name = "person",
                .description = "A person mentioned in the text.",
                .schema = moai::Json{
                    { "type", "object" },
                    { "properties", moai::Json{
                                        { "name", moai::Json{ { "type", "string" } } },
                                        { "birth_city", moai::Json{ { "type", "string" } } },
                                        { "birth_year", moai::Json{ { "type", "integer" } } },
                                    } },
                    { "required", moai::Json::array({ "name", "birth_city", "birth_year" }) },
                    { "additionalProperties", false },
                },
                .strict = true,
            },
        },
    }) };
    if (!completion) {
        std::println(std::cerr, "{}", completion.error());
        return 1;
    }

    auto const parsed{ moai::parse_structured_output(*completion) };
    if (!parsed) {
        // LengthFinishReason and ContentFilterFinishReason mean the model output was not usable.
        std::println(std::cerr, "{}", parsed.error());
        return 1;
    }
    std::println("{}", parsed->dump(2));
    return 0;
}
