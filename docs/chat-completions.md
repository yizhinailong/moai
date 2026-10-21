# Chat Completions

资源入口为 `client.chat.completions`，对应 `POST /chat/completions`。参数类型
`ChatCompletionCreateParams` 与官方 API 字段一一对应；未建模的新参数可通过
`RequestOptions::extra_body` 发送。

## 创建补全

```cpp
auto completion = client->chat.completions.Create({
    .model = "gpt-5-mini",
    .messages = {
        moai::developer_message("回答保持简短。"),
        moai::user_message("什么是 C++ 模块？"),
    },
    .temperature = 0.2,
    .max_completion_tokens = 200,
});
```

`Create` 阻塞到响应完成。成功时返回 `ChatCompletion`：

- `choices`：每个选项的 `message` 与 `finish_reason`
- `usage`：token 统计，如果服务端返回
- `raw`：完整响应 JSON，包含未建模字段

## 消息

`ChatCompletionMessageParam` 是 `std::variant`，包含 developer、system、user、assistant、tool
五种消息。辅助函数避免手写变体：

```cpp
moai::developer_message("...");
moai::system_message("...");
moai::user_message("纯文本");
moai::user_message(std::vector<moai::ChatCompletionContentPart>{ ... });
moai::assistant_message("较早的回复");
moai::tool_message("call_1", R"({"ok":true})");
```

多模态 user 消息使用内容片段：

```cpp
conversation.Add(moai::user_message(std::vector<moai::ChatCompletionContentPart>{
    moai::ChatCompletionContentPartText{ .text = "描述这张图片" },
    moai::ChatCompletionContentPartImage{ .image_url = { .url = "https://example.com/a.png", .detail = "high" } },
}));
```

也可使用 `ChatCompletionContentPartInputAudio` 和 `ChatCompletionContentPartFile`。字符串内容和
片段列表由同一个变体表示，未设置的 `name` 等可选字段不会序列化。

## 常用参数

| 字段 | 说明 |
| --- | --- |
| `messages` | 对话历史 |
| `tools` / `tool_choice` / `parallel_tool_calls` | 工具调用 |
| `response_format` | `text`、`json_object` 或 `json_schema` |
| `max_completion_tokens` | 输出上限，包含推理 token |
| `temperature`、`top_p`、`n` | 采样控制 |
| `stop` | 单个字符串或字符串列表 |
| `stream_options` | 仅用于流式请求，例如 `include_usage` |
| `metadata`、`store` | 已存储补全的元数据与保存开关 |
| `reasoning_effort`、`verbosity`、`service_tier` | 使用字符串，新取值无需等待库更新 |

`max_tokens`、`seed` 和 `user` 为兼容字段；`functions` / `function_call` 等已弃用接口不提供。
客户端不重复服务端对取值范围的校验。

## 工具调用

定义 function 工具：

```cpp
auto params = moai::ChatCompletionCreateParams{
    .model = "gpt-5-mini",
    .messages = { moai::user_message("北京现在天气如何？") },
    .tools = {
        moai::ChatCompletionFunctionTool{
            .function = {
                .name = "get_weather",
                .description = "查询指定城市的天气。",
                .parameters = moai::Json{
                    { "type", "object" },
                    { "properties", moai::Json{ { "city", moai::Json{ { "type", "string" } } } } },
                    { "required", moai::Json::array({ "city" }) },
                    { "additionalProperties", false },
                },
                .strict = true,
            },
        },
    },
};
```

模型返回 `finish_reason == "tool_calls"` 时，把 assistant 消息和工具结果都追加到历史，再次请求：

```cpp
moai::Conversation conversation;
conversation.Add(moai::user_message("北京现在天气如何？"));

params.messages = conversation.Messages(); // params 为上文定义了 tools 的请求参数
auto completion = client->chat.completions.Create(params);
if (completion) {
    conversation.Add(*completion);
    for (auto const& call : completion->choices.at(0).message.tool_calls) {
        if (auto const* function = std::get_if<moai::ChatCompletionMessageFunctionToolCall>(&call)) {
            auto arguments = moai::parse_tool_arguments(*function);
            if (!arguments) {
                conversation.AddToolResult(function->id, std::format(R"({{"error":"{}"}})", arguments.error().message));
                continue;
            }
            conversation.AddToolResult(function->id, call_my_function(function->function.name, *arguments));
        }
    }
    params.messages = conversation.Messages();
    completion = client->chat.completions.Create(params);
}
```

`parse_tool_arguments` 返回 `Json` 或 `Decode` 错误。custom 工具的 `input` 是自由文本，不适用该辅助。

`tool_choice` 可以是 `"none"`、`"auto"`、`"required"`、指定 function、指定 custom 工具或
`allowed_tools`：

```cpp
.tool_choice = moai::ChatCompletionNamedToolChoice{ .function = { .name = "get_weather" } }
```

## 结构化输出

使用 JSON Schema：

```cpp
auto completion = client->chat.completions.Create({
    .model = "gpt-5-mini",
    .messages = { moai::user_message("Ada Lovelace was born in London in 1815.") },
    .response_format = moai::ResponseFormatJsonSchema{
        .json_schema = {
            .name = "person",
            .schema = moai::Json{
                { "type", "object" },
                { "properties", moai::Json{
                    { "name", moai::Json{ { "type", "string" } } },
                    { "birth_year", moai::Json{ { "type", "integer" } } },
                } },
                { "required", moai::Json::array({ "name", "birth_year" }) },
                { "additionalProperties", false },
            },
            .strict = true,
        },
    },
});
```

解析结果：

```cpp
if (auto parsed = moai::parse_structured_output(*completion)) {
    std::println("{}", parsed->dump());
}
```

`parse_structured_output` 会：

1. 检查 choice 索引是否有效；
2. 把 `length` 和 `content_filter` 转为 `LengthFinishReason` / `ContentFilterFinishReason`；
3. 要求 `message.content` 存在；
4. 把内容解析为 `Json`，失败时返回 `Decode` 并保留原文到 `Error::body`。

只需要检查完成原因时使用 `moai::check_finish_reason(choice)`。若请求使用了 JSON 模式
（`ResponseFormatJsonObject`），服务端不保证输出是合法 JSON，应把 `Decode` 视为模型失败并重试。

## 对话历史 `Conversation`

`Conversation` 只保存请求消息，不绑定客户端，也不依赖 `ChatCompletionCreateParams`：

```cpp
moai::Conversation conversation;
conversation.Add(moai::system_message("回答保持简短。"));
conversation.Add(moai::user_message("什么是模块？"));

auto completion = client->chat.completions.Create({
    .model = "gpt-5-mini",
    .messages = conversation.Messages(),
});
if (completion) {
    conversation.Add(*completion);
}
```

- `Add(ChatCompletion const&, choice_index = 0)` 追加 assistant 消息，并保留工具调用
- `AddToolResult(id, content)` 回答工具调用
- `SetMessageLimit(n)` 限制消息数量，裁剪时保留开头的 developer / system 消息
- 裁剪包含工具调用的 assistant 消息时，会同时删除它对应的工具结果，避免出现孤立消息
- `ToJson()` / `FromJson()` 用于保存和恢复历史

```cpp
conversation.SetMessageLimit(40);
auto saved = conversation.ToJson();
auto restored = moai::Conversation::FromJson(saved);
```

`Messages()` 返回 `const&`，在下一次修改前有效。`Conversation` 不是线程安全的；多个线程共享时
需要自行加锁。

## 流式输出

```cpp
moai::ChatCompletionAccumulator accumulator;
auto result = client->chat.completions.CreateStream(params, [&](moai::ChatCompletionChunk const& chunk) {
    accumulator.Add(chunk);
    if (!chunk.choices.empty()) {
        std::print("{}", chunk.choices[0].delta.content.value_or(""));
    }
    return true;
});
```

回调返回 `false` 会中止传输并返回 `Cancelled`。详见[流式、异步与协程](streaming-async.md)。

## 已存储补全

创建时设置 `.store = true`，之后可以检索和管理：

```cpp
auto stored = client->chat.completions.Create({ .model = "gpt-5-mini", .messages = { ... }, .store = true });
auto id = stored->id;

auto fetched = client->chat.completions.Retrieve(id);
auto updated = client->chat.completions.Update(id, std::map<std::string, std::string>{ { "tag", "demo" } });
auto page = client->chat.completions.List({ .limit = 10, .order = "desc" });
auto messages = client->chat.completions.messages.List(id, { .limit = 20 });
auto deleted = client->chat.completions.Delete(id);
```

`Update` 的 `metadata` 为 `std::nullopt` 时按 API 要求发送 `null` 以清除元数据。

分页返回 `CursorPage<T>`：

```cpp
if (page) {
    for (auto const& item : page->data) { /* ... */ }
    if (page->HasNextPage()) {
        auto next = client->chat.completions.List({ .after = page->last_id, .limit = 10 });
    }
}
```

自动翻页辅助尚未提供。

## JSON 初始化注意

`moai::Json` 是 `nlohmann::json`，花括号会被当作初始化列表：

```cpp
moai::Json value{ other };      // 错误：单元素数组
moai::Json value = other;       // 正确：复制
moai::Json value(other);        // 正确：复制
```

只有书写对象或数组字面量时才使用花括号，例如 `moai::Json{ { "type", "object" } }`。
