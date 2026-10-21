# 配置与认证

## 创建客户端

`moai::Client::Create(options)` 解析配置并创建共享状态。缺少 API key 或参数非法时返回
`ErrorKind::InvalidArgument`。

```cpp
auto client = moai::Client::Create({
    .api_key = "sk-...",                       // 未设置时读取 OPENAI_API_KEY
    .organization = "org-...",                  // 可选
    .project = "proj_...",                      // 可选
    .base_url = "https://api.openai.com/v1",    // 兼容服务或本地 mock
    .timeout = std::chrono::seconds{ 600 },
    .connect_timeout = std::chrono::seconds{ 5 },
    .max_retries = 2,
});
```

### `ClientOptions`

| 字段 | 默认值 | 说明 |
| --- | --- | --- |
| `api_key` | `OPENAI_API_KEY` | 必填，作为 `Authorization: Bearer` 发送 |
| `organization` | `OPENAI_ORG_ID` | 发送为 `OpenAI-Organization` |
| `project` | `OPENAI_PROJECT_ID` | 发送为 `OpenAI-Project` |
| `base_url` | `https://api.openai.com/v1` | API 根地址，尾部斜杠会被去掉 |
| `timeout` | 600 s | 整个传输的限制；零表示不限制 |
| `connect_timeout` | 5 s | 建立连接的限制 |
| `max_retries` | 2 | 首次尝试之外的失败重试次数；零表示不重试 |
| `default_headers` | 空 | 所有请求附加的请求头，可覆盖内置请求头 |
| `default_query` | 空 | 所有请求附加的查询参数 |
| `proxies` | 空 | 协议到代理地址的映射，例如 `{"https", "http://127.0.0.1:7890"}` |

环境变量只在对应字段未设置时读取，空字符串视为未设置。显式配置优先。

`Client` 创建后配置不可变；复制 `Client` 共享同一份配置和连接池。

### 认证

采用 Bearer token：

```text
Authorization: Bearer <api_key>
```

库不会在错误消息、日志或异常中写入 API key。示例和测试也不要提交真实密钥。密钥目前以
`std::string` 保存，尚未使用 mcr 的 `SecureString`。

## 单次请求选项

`RequestOptions` 只作用于一次调用；字段在调用时复制，不会借用调用方的对象。

| 字段 | 说明 |
| --- | --- |
| `timeout` | 覆盖 `ClientOptions::timeout` |
| `max_retries` | 覆盖 `ClientOptions::max_retries` |
| `client_request_id` | 发送为 `X-Client-Request-Id`，便于服务端排查问题 |
| `extra_headers` | 附加或覆盖请求头，名称不区分大小写 |
| `extra_query` | 追加查询参数 |
| `extra_body` | JSON 对象，合并进请求体并覆盖同名字段 |
| `stop_token` | 取消传输和重试等待，见[流式、异步与协程](streaming-async.md) |

```cpp
auto completion = client->chat.completions.Create(
    params,
    {
        .timeout = std::chrono::seconds{ 30 },
        .client_request_id = "batch-42-item-7",
        .extra_headers = { { "X-Trace", "abc" } },
        .extra_query = { { "trace", "on" } },
    }
);
```

`extra_body` 用于发送尚未建模的新参数，或临时覆盖已建模字段：

```cpp
auto completion = client->chat.completions.Create(
    params,
    { .extra_body = moai::Json{ { "new_option", true } } }
);
```

`extra_body` 必须是 JSON 对象；非 POST 请求携带请求体会返回 `InvalidArgument`。

## 请求头

每次请求默认发送：

- `Authorization`、`OpenAI-Organization`、`OpenAI-Project`（按配置）
- `Content-Type: application/json`（有请求体时）
- `Accept: application/json`，流式请求为 `text/event-stream`
- `User-Agent: moai/<VERSION>`

`default_headers` 先应用，`extra_headers` 最后应用，因此单次请求可以覆盖任何同名请求头。

## 基础地址与代理

`base_url` 指向兼容服务时只需替换根地址，路径和查询参数由各资源自行拼接：

```cpp
auto client = moai::Client::Create({
    .api_key = "sk-test",
    .base_url = "http://127.0.0.1:8080/v1",
});
```

代理按协议配置，由 mcr 在传输层应用：

```cpp
auto client = moai::Client::Create({
    .api_key = "sk-...",
    .proxies = { { "https", "http://127.0.0.1:7890" } },
});
```

TLS 校验、CA 文件等选项尚未透传到 mcr 的 `SslOptions`；需要自定义 TLS 时请使用 mcr 或
系统信任库配置。
