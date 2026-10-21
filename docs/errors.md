# 错误处理

## `Result` 与异常

所有公开操作返回 `moai::Result<T>`，即 `std::expected<T, moai::Error>`。库本身不抛异常；
只有调用方通过流式回调抛出的异常会按原样传出。

```cpp
auto completion = client->chat.completions.Create(params);
if (!completion) {
    std::println("{}", completion.error());
    return 1;
}
auto const& message = completion->choices.at(0).message;
```

## `ErrorKind`

| 取值 | 含义 |
| --- | --- |
| `InvalidArgument` | 参数或请求体无法构造，例如选项非法、序列化失败、ID 为空 |
| `RequestSetup` | mcr 在传输前拒绝了配置 |
| `Connection` | 没有 HTTP 响应的传输失败，例如 DNS、TLS 或连接重置 |
| `Timeout` | 超过超时时间 |
| `Cancelled` | 调用方取消，或流式回调返回 `false` |
| `BadRequest` | HTTP 400 |
| `Authentication` | HTTP 401 |
| `PermissionDenied` | HTTP 403 |
| `NotFound` | HTTP 404 |
| `Conflict` | HTTP 409 |
| `UnprocessableEntity` | HTTP 422 |
| `RateLimit` | HTTP 429 |
| `InternalServer` | HTTP 500 及以上 |
| `Status` | 其他非 2xx 状态 |
| `Decode` | 响应体不是预期的 JSON 结构 |
| `Stream` | 流报告错误、提前结束，或响应不是事件流 |
| `LengthFinishReason` | 模型因达到输出 token 上限而停止 |
| `ContentFilterFinishReason` | 内容过滤器阻止了模型输出 |

`Error::IsStatusError()` 对 `BadRequest` 到 `Status` 返回 `true`，这些错误对应真实的 HTTP 响应。

## `Error` 字段

| 字段 | 说明 |
| --- | --- |
| `kind` | 错误类别 |
| `message` | 可读的诊断信息 |
| `status_code` | HTTP 状态码；没有响应时为零 |
| `request_id` | 响应头 `x-request-id`，如果服务端返回 |
| `type` / `code` / `param` | API 错误体中的同名字段 |
| `body` | 原始错误响应体或流事件，最多保留 64 KiB |
| `transport_code` | 传输错误的 mcr `ErrorCode` |

`Error` 提供 `std::formatter`：

```text
RateLimit (HTTP 429, request req_1): ...
```

非标准错误体也能识别：`{"error": {"message": ...}}`、`{"error": "text"}`、`{"detail": ...}`、
非 JSON 正文和空正文都会保留可用的信息。

## 重试

默认 `max_retries = 2`，即最多尝试三次。以下失败会重试：

- 连接错误和超时
- HTTP 408、409、429 和 5xx
- 响应头 `x-should-retry: true` 时；`false` 则禁止重试

重试等待优先采用服务端要求：

- `retry-after-ms`：毫秒数
- `retry-after`：秒数或 IMF-fixdate 格式的 HTTP 日期（如 `Sun, 06 Nov 1994 08:49:37 GMT`）

服务端请求的等待超过 60 秒时不采用；否则使用从 0.5 秒开始、上限 8 秒的随机化指数退避。
请求体只序列化一次并在重试时重放。流式请求在交付首个分块后不再重试。

`RequestOptions::stop_token` 会中断重试等待并返回 `Cancelled`。

## 解码错误

响应字段缺失或为 `null` 时使用默认值；类型错误返回 `Decode`，消息中包含 JSON 路径，例如：

```text
choices[0].message.content: expected string, got number
```

成功响应中未建模的字段不会被丢弃，可从 `ChatCompletion::raw` 读取。

## 流式错误

流式调用返回第一个错误：

- 载荷包含 `error` 键：`ErrorKind::Stream`，并保留 `message` / `type` / `code`
- 载荷不是合法 JSON：`Decode`
- 2xx 但不是 `text/event-stream`：`Stream`
- 未收到 `data: [DONE]` 就结束：`Stream`
- 回调返回 `false`：`Cancelled`
- 非 2xx：对应的 HTTP 错误，错误响应体保留在 `body`

## 结构化输出的完成原因

`check_finish_reason` 和 `parse_structured_output` 会把不可用的输出转换为专门错误：

```cpp
auto parsed = moai::parse_structured_output(*completion);
if (!parsed) {
    if (parsed.error().kind == moai::ErrorKind::LengthFinishReason) {
        // 输出被截断，通常是 JSON 不完整
    }
}
```

`tool_calls`、`stop` 以及其他未知完成原因不会被拒绝，调用方可以自行判断。
