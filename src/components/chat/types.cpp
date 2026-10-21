/**
 * @file types.cpp
 * @brief JSON serialization of Chat Completions requests and decoding of responses.
 */
module moai;

import std;
import :json;
import :chat_types;

namespace moai {

    namespace {

        /**
         * @brief Helper for std::visit with overloaded lambdas.
         */
        template <typename... Ts>
        struct Overloaded : Ts... {
            using Ts::operator()...;
        };

        auto encode(ChatCompletionContentPartText const& part) -> Json {
            return Json{ { "type", "text" }, { "text", part.text } };
        }

        auto encode(ChatCompletionContentPartRefusal const& part) -> Json {
            return Json{ { "type", "refusal" }, { "refusal", part.refusal } };
        }

        auto encode(ChatCompletionContentPartImage const& part) -> Json {
            Json image_url = Json::object();
            image_url["url"] = part.image_url.url;
            detail::put_optional(image_url, "detail", part.image_url.detail);
            return Json{ { "type", "image_url" }, { "image_url", std::move(image_url) } };
        }

        auto encode(ChatCompletionContentPartInputAudio const& part) -> Json {
            return Json{
                { "type", "input_audio" },
                { "input_audio", Json{ { "data", part.input_audio.data }, { "format", part.input_audio.format } } },
            };
        }

        auto encode(ChatCompletionContentPartFile const& part) -> Json {
            Json file = Json::object();
            detail::put_optional(file, "file_data", part.file.file_data);
            detail::put_optional(file, "file_id", part.file.file_id);
            detail::put_optional(file, "filename", part.file.filename);
            return Json{ { "type", "file" }, { "file", std::move(file) } };
        }

        auto encode(ChatCompletionContentPart const& part) -> Json {
            return std::visit([](auto const& alternative) { return encode(alternative); }, part);
        }

        auto encode(ChatCompletionAssistantContentPart const& part) -> Json {
            return std::visit([](auto const& alternative) { return encode(alternative); }, part);
        }

        /**
         * @brief Encode a string or a list of parts.
         */
        template <typename Part>
        auto encode_content(std::variant<std::string, std::vector<Part>> const& content) -> Json {
            if (auto const* text{ std::get_if<std::string>(&content) }) {
                return *text;
            }
            Json parts = Json::array();
            for (auto const& part : std::get<std::vector<Part>>(content)) {
                parts.push_back(encode(part));
            }
            return parts;
        }

        auto encode(ChatCompletionMessageToolCall const& call) -> Json {
            return std::visit(
                Overloaded{
                    [](ChatCompletionMessageFunctionToolCall const& function) {
                        return Json{
                            { "id", function.id },
                            { "type", "function" },
                            { "function", Json{ { "name", function.function.name }, { "arguments", function.function.arguments } } },
                        };
                    },
                    [](ChatCompletionMessageCustomToolCall const& custom) {
                        return Json{
                            { "id", custom.id },
                            { "type", "custom" },
                            { "custom", Json{ { "name", custom.custom.name }, { "input", custom.custom.input } } },
                        };
                    },
                },
                call
            );
        }

        /**
         * @brief Encode a message with a role, content, and optional name.
         */
        template <typename Content>
        auto encode_named(std::string_view role, Content const& content, std::optional<std::string> const& name) -> Json {
            Json message = Json::object();
            message["role"] = role;
            message["content"] = encode_content(content);
            detail::put_optional(message, "name", name);
            return message;
        }

        auto encode(ChatCompletionMessageParam const& message) -> Json {
            return std::visit(
                Overloaded{
                    [](ChatCompletionDeveloperMessageParam const& value) { return encode_named("developer", value.content, value.name); },
                    [](ChatCompletionSystemMessageParam const& value) { return encode_named("system", value.content, value.name); },
                    [](ChatCompletionUserMessageParam const& value) { return encode_named("user", value.content, value.name); },
                    [](ChatCompletionAssistantMessageParam const& value) {
                        Json result = Json::object();
                        result["role"] = "assistant";
                        if (value.content) {
                            result["content"] = encode_content(*value.content);
                        }
                        detail::put_optional(result, "refusal", value.refusal);
                        detail::put_optional(result, "name", value.name);
                        if (!value.tool_calls.empty()) {
                            Json calls = Json::array();
                            for (auto const& call : value.tool_calls) {
                                calls.push_back(encode(call));
                            }
                            result["tool_calls"] = std::move(calls);
                        }
                        if (value.audio) {
                            result["audio"] = Json{ { "id", value.audio->id } };
                        }
                        return result;
                    },
                    [](ChatCompletionToolMessageParam const& value) {
                        Json result = Json::object();
                        result["role"] = "tool";
                        result["content"] = encode_content(value.content);
                        result["tool_call_id"] = value.tool_call_id;
                        return result;
                    },
                },
                message
            );
        }

        auto encode(ChatCompletionTool const& tool) -> Json {
            return std::visit(
                Overloaded{
                    [](ChatCompletionFunctionTool const& value) {
                        Json function = Json::object();
                        function["name"] = value.function.name;
                        detail::put_optional(function, "description", value.function.description);
                        detail::put_optional(function, "parameters", value.function.parameters);
                        detail::put_optional(function, "strict", value.function.strict);
                        return Json{ { "type", "function" }, { "function", std::move(function) } };
                    },
                    [](ChatCompletionCustomTool const& value) {
                        Json custom = Json::object();
                        custom["name"] = value.custom.name;
                        detail::put_optional(custom, "description", value.custom.description);
                        if (value.custom.format) {
                            custom["format"] = std::visit(
                                Overloaded{
                                    [](CustomToolInputFormatText const&) { return Json{ { "type", "text" } }; },
                                    [](CustomToolInputFormatGrammar const& grammar) {
                                        return Json{
                                            { "type", "grammar" },
                                            { "grammar", Json{ { "definition", grammar.grammar.definition }, { "syntax", grammar.grammar.syntax } } },
                                        };
                                    },
                                },
                                *value.custom.format
                            );
                        }
                        return Json{ { "type", "custom" }, { "custom", std::move(custom) } };
                    },
                },
                tool
            );
        }

        auto encode(ChatCompletionToolChoiceOption const& choice) -> Json {
            return std::visit(
                Overloaded{
                    [](std::string const& mode) { return Json(mode); },
                    [](ChatCompletionNamedToolChoice const& value) {
                        return Json{ { "type", "function" }, { "function", Json{ { "name", value.function.name } } } };
                    },
                    [](ChatCompletionNamedToolChoiceCustom const& value) {
                        return Json{ { "type", "custom" }, { "custom", Json{ { "name", value.custom.name } } } };
                    },
                    [](ChatCompletionAllowedToolChoice const& value) {
                        return Json{
                            { "type", "allowed_tools" },
                            { "allowed_tools", Json{ { "mode", value.allowed_tools.mode }, { "tools", value.allowed_tools.tools } } },
                        };
                    },
                },
                choice
            );
        }

        auto encode(ResponseFormat const& format) -> Json {
            return std::visit(
                Overloaded{
                    [](ResponseFormatText const&) { return Json{ { "type", "text" } }; },
                    [](ResponseFormatJsonObject const&) { return Json{ { "type", "json_object" } }; },
                    [](ResponseFormatJsonSchema const& value) {
                        Json schema = Json::object();
                        schema["name"] = value.json_schema.name;
                        detail::put_optional(schema, "description", value.json_schema.description);
                        detail::put_optional(schema, "schema", value.json_schema.schema);
                        detail::put_optional(schema, "strict", value.json_schema.strict);
                        return Json{ { "type", "json_schema" }, { "json_schema", std::move(schema) } };
                    },
                },
                format
            );
        }

        auto encode(WebSearchOptions const& options) -> Json {
            Json result = Json::object();
            detail::put_optional(result, "search_context_size", options.search_context_size);
            if (options.user_location) {
                Json approximate = Json::object();
                auto const& location{ options.user_location->approximate };
                detail::put_optional(approximate, "city", location.city);
                detail::put_optional(approximate, "country", location.country);
                detail::put_optional(approximate, "region", location.region);
                detail::put_optional(approximate, "timezone", location.timezone);
                result["user_location"] = Json{ { "type", "approximate" }, { "approximate", std::move(approximate) } };
            }
            return result;
        }

        // ------------------------------------------------------------------------------------
        // Decoding
        // ------------------------------------------------------------------------------------

        using detail::DecodeFailure;
        using detail::JsonReader;

        auto decode_content_part(JsonReader const& reader) -> ChatCompletionContentPart {
            reader.RequireObject();
            auto const type{ reader.StringOr("type") };
            if (type == "text") {
                return ChatCompletionContentPartText{ .text = reader.StringOr("text") };
            }
            if (type == "image_url") {
                ChatCompletionContentPartImage part;
                if (auto const image{ reader.Field("image_url") }) {
                    part.image_url.url = image->StringOr("url");
                    part.image_url.detail = image->OptionalString("detail");
                }
                return part;
            }
            if (type == "input_audio") {
                ChatCompletionContentPartInputAudio part;
                if (auto const audio{ reader.Field("input_audio") }) {
                    part.input_audio.data = audio->StringOr("data");
                    part.input_audio.format = audio->StringOr("format");
                }
                return part;
            }
            if (type == "file") {
                ChatCompletionContentPartFile part;
                if (auto const file{ reader.Field("file") }) {
                    part.file.file_data = file->OptionalString("file_data");
                    part.file.file_id = file->OptionalString("file_id");
                    part.file.filename = file->OptionalString("filename");
                }
                return part;
            }
            throw DecodeFailure{ std::format("{}: unsupported content part type '{}'", reader.Path(), type) };
        }

        auto decode_tool_call(JsonReader const& reader) -> ChatCompletionMessageToolCall {
            reader.RequireObject();
            auto const type{ reader.OptionalString("type") };
            auto const function{ reader.Field("function") };
            if (type == "function" || (!type && function)) {
                ChatCompletionMessageFunctionToolCall call{ .id = reader.StringOr("id") };
                if (function) {
                    call.function.name = function->StringOr("name");
                    call.function.arguments = function->StringOr("arguments");
                }
                return call;
            }
            if (type == "custom") {
                ChatCompletionMessageCustomToolCall call{ .id = reader.StringOr("id") };
                if (auto const custom{ reader.Field("custom") }) {
                    call.custom.name = custom->StringOr("name");
                    call.custom.input = custom->StringOr("input");
                }
                return call;
            }
            throw DecodeFailure{ std::format("{}: unsupported tool call type '{}'", reader.Path(), type.value_or("")) };
        }

        auto decode_token_logprob(JsonReader const& reader) -> ChatCompletionTokenLogprob {
            auto const bytes{ [](JsonReader const& parent) -> std::optional<std::vector<std::int64_t>> {
                if (auto const field{ parent.Field("bytes") }) {
                    return field->Array([](JsonReader const& item) { return item.Int(); });
                }
                return std::nullopt;
            } };
            return ChatCompletionTokenLogprob{
                .token = reader.StringOr("token"),
                .bytes = bytes(reader),
                .logprob = reader.OptionalNumber("logprob").value_or(0.0),
                .top_logprobs = reader.ArrayOr("top_logprobs", [&](JsonReader const& item) {
                    return ChatCompletionTokenLogprob::TopLogprob{
                        .token = item.StringOr("token"),
                        .bytes = bytes(item),
                        .logprob = item.OptionalNumber("logprob").value_or(0.0),
                    };
                }),
            };
        }

        auto decode_logprobs(JsonReader const& reader) -> ChatCompletionChoiceLogprobs {
            ChatCompletionChoiceLogprobs result;
            if (auto const content{ reader.Field("content") }) {
                result.content = content->Array(decode_token_logprob);
            }
            if (auto const refusal{ reader.Field("refusal") }) {
                result.refusal = refusal->Array(decode_token_logprob);
            }
            return result;
        }

        auto decode_usage(JsonReader const& reader) -> CompletionUsage {
            CompletionUsage usage{
                .completion_tokens = reader.IntOr("completion_tokens"),
                .prompt_tokens = reader.IntOr("prompt_tokens"),
                .total_tokens = reader.IntOr("total_tokens"),
            };
            if (auto const details{ reader.Field("completion_tokens_details") }) {
                usage.completion_tokens_details = CompletionUsage::CompletionTokensDetails{
                    .accepted_prediction_tokens = details->OptionalInt("accepted_prediction_tokens"),
                    .audio_tokens = details->OptionalInt("audio_tokens"),
                    .reasoning_tokens = details->OptionalInt("reasoning_tokens"),
                    .rejected_prediction_tokens = details->OptionalInt("rejected_prediction_tokens"),
                };
            }
            if (auto const details{ reader.Field("prompt_tokens_details") }) {
                usage.prompt_tokens_details = CompletionUsage::PromptTokensDetails{
                    .audio_tokens = details->OptionalInt("audio_tokens"),
                    .cached_tokens = details->OptionalInt("cached_tokens"),
                };
            }
            return usage;
        }

        auto decode_message(JsonReader const& reader) -> ChatCompletionMessage {
            reader.RequireObject();
            ChatCompletionMessage message{
                .role = reader.OptionalString("role").value_or("assistant"),
                .content = reader.OptionalString("content"),
                .refusal = reader.OptionalString("refusal"),
                .annotations = reader.ArrayOr("annotations", [](JsonReader const& item) {
                    ChatCompletionMessage::Annotation annotation{ .type = item.OptionalString("type").value_or("url_citation") };
                    if (auto const citation{ item.Field("url_citation") }) {
                        annotation.url_citation = ChatCompletionMessage::Annotation::UrlCitation{
                            .end_index = citation->IntOr("end_index"),
                            .start_index = citation->IntOr("start_index"),
                            .title = citation->StringOr("title"),
                            .url = citation->StringOr("url"),
                        };
                    }
                    return annotation;
                }),
                .tool_calls = reader.ArrayOr("tool_calls", decode_tool_call),
            };
            if (auto const audio{ reader.Field("audio") }) {
                message.audio = ChatCompletionAudio{
                    .id = audio->StringOr("id"),
                    .data = audio->StringOr("data"),
                    .expires_at = audio->IntOr("expires_at"),
                    .transcript = audio->StringOr("transcript"),
                };
            }
            return message;
        }

        /**
         * @brief Decode an optional nested object with the given decoder.
         */
        template <typename Decode>
        auto decode_optional(JsonReader const& reader, std::string_view key, Decode&& decode) -> std::optional<std::invoke_result_t<Decode&, JsonReader const&>> {
            if (auto const field{ reader.Field(key) }) {
                return decode(*field);
            }
            return std::nullopt;
        }

        /**
         * @brief Decode a text part of a request message.
         */
        auto decode_text_part(JsonReader const& reader) -> ChatCompletionContentPartText {
            reader.RequireObject();
            auto const type{ reader.StringOr("type") };
            if (type != "text") {
                throw DecodeFailure{ std::format("{}: unsupported text content part type '{}'", reader.Path(), type) };
            }
            return ChatCompletionContentPartText{ .text = reader.StringOr("text") };
        }

        /**
         * @brief Decode required text content given as a string or a list of text parts.
         */
        auto decode_text_content(JsonReader const& reader) -> ChatCompletionTextContent {
            auto const content{ reader.Field("content") };
            if (!content) {
                throw DecodeFailure{ std::format("{}: missing message content", reader.Path().empty() ? "<root>" : reader.Path()) };
            }
            if (content->Value().is_string()) {
                return content->String();
            }
            return content->Array(decode_text_part);
        }

        /**
         * @brief Decode a content part of an assistant request message.
         */
        auto decode_assistant_part(JsonReader const& reader) -> ChatCompletionAssistantContentPart {
            reader.RequireObject();
            auto const type{ reader.StringOr("type") };
            if (type == "text") {
                return ChatCompletionContentPartText{ .text = reader.StringOr("text") };
            }
            if (type == "refusal") {
                return ChatCompletionContentPartRefusal{ .refusal = reader.StringOr("refusal") };
            }
            throw DecodeFailure{ std::format("{}: unsupported assistant content part type '{}'", reader.Path(), type) };
        }

        /**
         * @brief Decode optional assistant content given as a string or a list of parts.
         */
        auto decode_assistant_content(JsonReader const& content) -> ChatCompletionAssistantContent {
            if (content.Value().is_string()) {
                return content.String();
            }
            return content.Array(decode_assistant_part);
        }

    } // namespace

    auto developer_message(std::string content) -> ChatCompletionMessageParam {
        return ChatCompletionDeveloperMessageParam{ .content = std::move(content) };
    }

    auto system_message(std::string content) -> ChatCompletionMessageParam {
        return ChatCompletionSystemMessageParam{ .content = std::move(content) };
    }

    auto user_message(std::string content) -> ChatCompletionMessageParam {
        return ChatCompletionUserMessageParam{ .content = std::move(content) };
    }

    auto user_message(std::vector<ChatCompletionContentPart> parts) -> ChatCompletionMessageParam {
        return ChatCompletionUserMessageParam{ .content = std::move(parts) };
    }

    auto assistant_message(std::string content) -> ChatCompletionMessageParam {
        return ChatCompletionAssistantMessageParam{ .content = ChatCompletionAssistantContent{ std::move(content) } };
    }

    auto assistant_message(ChatCompletionMessage const& message) -> ChatCompletionMessageParam {
        ChatCompletionAssistantMessageParam result{ .refusal = message.refusal, .tool_calls = message.tool_calls };
        if (message.content) {
            result.content = ChatCompletionAssistantContent{ *message.content };
        }
        if (message.audio) {
            result.audio = ChatCompletionAssistantMessageParam::Audio{ .id = message.audio->id };
        }
        return result;
    }

    auto tool_message(std::string tool_call_id, std::string content) -> ChatCompletionMessageParam {
        return ChatCompletionToolMessageParam{ .content = std::move(content), .tool_call_id = std::move(tool_call_id) };
    }

} // namespace moai

namespace moai::detail {

    auto encode_create_params(ChatCompletionCreateParams const& params) -> Json {
        Json body = Json::object();
        body["model"] = params.model;
        Json messages = Json::array();
        for (auto const& message : params.messages) {
            messages.push_back(encode(message));
        }
        body["messages"] = std::move(messages);
        if (params.audio) {
            body["audio"] = Json{ { "format", params.audio->format }, { "voice", params.audio->voice } };
        }
        put_optional(body, "frequency_penalty", params.frequency_penalty);
        put_optional(body, "logit_bias", params.logit_bias);
        put_optional(body, "logprobs", params.logprobs);
        put_optional(body, "max_completion_tokens", params.max_completion_tokens);
        put_optional(body, "max_tokens", params.max_tokens);
        put_optional(body, "metadata", params.metadata);
        put_optional(body, "modalities", params.modalities);
        put_optional(body, "n", params.n);
        put_optional(body, "parallel_tool_calls", params.parallel_tool_calls);
        if (params.prediction) {
            body["prediction"] = Json{ { "type", "content" }, { "content", encode_content(params.prediction->content) } };
        }
        put_optional(body, "presence_penalty", params.presence_penalty);
        put_optional(body, "prompt_cache_key", params.prompt_cache_key);
        put_optional(body, "reasoning_effort", params.reasoning_effort);
        if (params.response_format) {
            body["response_format"] = encode(*params.response_format);
        }
        put_optional(body, "safety_identifier", params.safety_identifier);
        put_optional(body, "seed", params.seed);
        put_optional(body, "service_tier", params.service_tier);
        if (params.stop) {
            body["stop"] = std::visit([](auto const& stop) { return Json(stop); }, *params.stop);
        }
        put_optional(body, "store", params.store);
        if (params.stream_options) {
            Json options = Json::object();
            put_optional(options, "include_obfuscation", params.stream_options->include_obfuscation);
            put_optional(options, "include_usage", params.stream_options->include_usage);
            body["stream_options"] = std::move(options);
        }
        put_optional(body, "temperature", params.temperature);
        if (params.tool_choice) {
            body["tool_choice"] = encode(*params.tool_choice);
        }
        if (!params.tools.empty()) {
            Json tools = Json::array();
            for (auto const& tool : params.tools) {
                tools.push_back(encode(tool));
            }
            body["tools"] = std::move(tools);
        }
        put_optional(body, "top_logprobs", params.top_logprobs);
        put_optional(body, "top_p", params.top_p);
        put_optional(body, "user", params.user);
        put_optional(body, "verbosity", params.verbosity);
        if (params.web_search_options) {
            body["web_search_options"] = encode(*params.web_search_options);
        }
        return body;
    }

    auto encode_message(ChatCompletionMessageParam const& message) -> Json {
        return encode(message);
    }

    auto decode_message_param(JsonReader const& reader) -> ChatCompletionMessageParam {
        reader.RequireObject();
        auto const role{ reader.OptionalString("role") };
        if (!role) {
            throw DecodeFailure{ std::format("{}: missing message role", reader.Path().empty() ? "<root>" : reader.Path()) };
        }
        if (*role == "developer") {
            return ChatCompletionDeveloperMessageParam{
                .content = decode_text_content(reader),
                .name = reader.OptionalString("name"),
            };
        }
        if (*role == "system") {
            return ChatCompletionSystemMessageParam{
                .content = decode_text_content(reader),
                .name = reader.OptionalString("name"),
            };
        }
        if (*role == "user") {
            auto const content{ reader.Field("content") };
            if (!content) {
                throw DecodeFailure{ std::format("{}: missing message content", reader.Path().empty() ? "<root>" : reader.Path()) };
            }
            return ChatCompletionUserMessageParam{
                .content = content->Value().is_string() ? ChatCompletionUserContent{ content->String() } : ChatCompletionUserContent{ content->Array(decode_content_part) },
                .name = reader.OptionalString("name"),
            };
        }
        if (*role == "assistant") {
            ChatCompletionAssistantMessageParam message{
                .refusal = reader.OptionalString("refusal"),
                .name = reader.OptionalString("name"),
                .tool_calls = reader.ArrayOr("tool_calls", decode_tool_call),
            };
            if (auto const content{ reader.Field("content") }) {
                message.content = decode_assistant_content(*content);
            }
            if (auto const audio{ reader.Field("audio") }) {
                message.audio = ChatCompletionAssistantMessageParam::Audio{ .id = audio->StringOr("id") };
            }
            return message;
        }
        if (*role == "tool") {
            return ChatCompletionToolMessageParam{
                .content = decode_text_content(reader),
                .tool_call_id = reader.StringOr("tool_call_id"),
            };
        }
        throw DecodeFailure{ std::format("{}: unsupported message role '{}'", reader.Path().empty() ? "<root>" : reader.Path(), *role) };
    }

    auto decode_chat_completion(JsonReader const& reader) -> ChatCompletion {
        reader.RequireObject();
        return ChatCompletion{
            .id = reader.StringOr("id"),
            .choices = reader.ArrayOr("choices", [](JsonReader const& item) {
                item.RequireObject();
                ChatCompletion::Choice choice{
                    .finish_reason = item.StringOr("finish_reason"),
                    .index = item.IntOr("index"),
                    .logprobs = decode_optional(item, "logprobs", decode_logprobs),
                };
                if (auto const message{ item.Field("message") }) {
                    choice.message = decode_message(*message);
                }
                return choice;
            }),
            .created = reader.IntOr("created"),
            .model = reader.StringOr("model"),
            .object = reader.OptionalString("object").value_or("chat.completion"),
            .service_tier = reader.OptionalString("service_tier"),
            .system_fingerprint = reader.OptionalString("system_fingerprint"),
            .usage = decode_optional(reader, "usage", decode_usage),
            .raw = reader.Value(),
        };
    }

    auto decode_chat_completion_chunk(JsonReader const& reader) -> ChatCompletionChunk {
        reader.RequireObject();
        return ChatCompletionChunk{
            .id = reader.StringOr("id"),
            .choices = reader.ArrayOr("choices", [](JsonReader const& item) {
                item.RequireObject();
                ChatCompletionChunk::Choice choice{
                    .finish_reason = item.OptionalString("finish_reason"),
                    .index = item.IntOr("index"),
                    .logprobs = decode_optional(item, "logprobs", decode_logprobs),
                };
                if (auto const delta{ item.Field("delta") }) {
                    choice.delta = ChatCompletionChunk::Choice::Delta{
                        .content = delta->OptionalString("content"),
                        .refusal = delta->OptionalString("refusal"),
                        .role = delta->OptionalString("role"),
                        .tool_calls = delta->ArrayOr("tool_calls", [](JsonReader const& call) {
                            using ToolCall = ChatCompletionChunk::Choice::Delta::ToolCall;
                            return ToolCall{
                                .index = call.IntOr("index"),
                                .id = call.OptionalString("id"),
                                .function = decode_optional(call, "function", [](JsonReader const& function) {
                                    return ToolCall::Function{
                                        .arguments = function.OptionalString("arguments"),
                                        .name = function.OptionalString("name"),
                                    };
                                }),
                                .type = call.OptionalString("type"),
                            };
                        }),
                    };
                }
                return choice;
            }),
            .created = reader.IntOr("created"),
            .model = reader.StringOr("model"),
            .object = reader.OptionalString("object").value_or("chat.completion.chunk"),
            .service_tier = reader.OptionalString("service_tier"),
            .system_fingerprint = reader.OptionalString("system_fingerprint"),
            .usage = decode_optional(reader, "usage", decode_usage),
        };
    }

    auto decode_chat_completion_deleted(JsonReader const& reader) -> ChatCompletionDeleted {
        reader.RequireObject();
        return ChatCompletionDeleted{
            .id = reader.StringOr("id"),
            .deleted = reader.OptionalBool("deleted").value_or(false),
            .object = reader.OptionalString("object").value_or("chat.completion.deleted"),
        };
    }

    auto decode_store_message(JsonReader const& reader) -> ChatCompletionStoreMessage {
        reader.RequireObject();
        ChatCompletionStoreMessage message{
            .id = reader.StringOr("id"),
            .role = reader.StringOr("role"),
            .content = reader.OptionalString("content"),
            .name = reader.OptionalString("name"),
            .refusal = reader.OptionalString("refusal"),
            .tool_calls = reader.ArrayOr("tool_calls", decode_tool_call),
            .tool_call_id = reader.OptionalString("tool_call_id"),
        };
        if (auto const parts{ reader.Field("content_parts") }) {
            message.content_parts = parts->Array(decode_content_part);
        }
        return message;
    }

} // namespace moai::detail
