# moai

基于 [mcr](https://github.com/yizhinailong/mcr)（0.3.0 及以上，Git `main` 分支）的 C++23
模块化 OpenAI 客户端库，使用 [mcpp](https://github.com/mcpp-community/mcpp) 构建和管理依赖。

代码组织参考 [liboai](https://github.com/D7EAD/liboai) 的统一客户端、核心层和资源组件，
功能设计对齐 [OpenAI 官方 SDK](https://developers.openai.com/api/reference/python)
和 [API 文档](https://developers.openai.com/api/reference/overview)。这是非官方 C++ 实现。

## 当前状态

当前版本为 `0.1.0`。已实现核心层和 Chat Completions 的同步、线程池异步、协程、SSE 流式、
已存储补全管理，以及对话历史、结构化输出和工具参数辅助，可以通过 `import moai;` 调用模型。
其他资源尚未开始。

liboai 已归档，本项目借鉴其代码分层，具体端点和行为以 OpenAI 官方资料为准。

| 阶段 | 内容 | 状态 |
| --- | --- | --- |
| 核心层 | 客户端配置、API key、base URL、环境变量、JSON、错误映射、超时、重试、取消 | 已实现 |
| Chat Completions | `Create`、`CreateStream`、流式分块合并、工具调用、结构化输出参数、多模态消息 | 已实现 |
| Chat Completions | 已存储补全的 `Retrieve` / `Update` / `List` / `Delete` / `messages.List` 及分页 | 已实现 |
| Chat Completions | 线程池异步 `CreateAsync` / `CreateStreamAsync`，协程 `CreateCoro` / `CreateStreamCoro` | 已实现 |
| Chat Completions | 对话历史 `Conversation`、结构化输出与工具参数辅助 | 已实现 |
| 首批扩展 | Models、Responses | 待实现 |
| 常用资源 | Embeddings、Images、Audio、Files、Uploads、Moderations | 待实现 |
| 扩展资源 | Conversations、Batches、Fine-tuning、Vector Stores | 待实现 |
| 后续评估 | Realtime/WebSocket、Webhooks 及其他官方资源 | 待设计 |

OpenAI 推荐用于新功能的 Responses 排在 Chat Completions 之后。尚未建模的新参数可通过
`RequestOptions::extra_body` 发送，响应中的未建模字段可从 `ChatCompletion::raw` 读取。

## 快速开始

```cpp
import std;
import moai;

auto main() -> int {
    auto client = moai::Client::Create(); // 读取 OPENAI_API_KEY，可选 OPENAI_BASE_URL
    if (!client) {
        std::println("{}", client.error());
        return 1;
    }
    auto completion = client->chat.completions.Create({
        .model = "gpt-5-mini",
        .messages = {
            moai::developer_message("Answer in one sentence."),
            moai::user_message("What is a C++ module?"),
        },
    });
    if (!completion) {
        std::println("{}", completion.error()); // 例如 "RateLimit (HTTP 429, request req_...): ..."
        return 1;
    }
    std::println("{}", completion->choices.at(0).message.content.value_or(""));
}
```

流式输出通过回调逐块交付，`ChatCompletionAccumulator` 可将分块合并为完整结果：

```cpp
moai::ChatCompletionAccumulator accumulator;
auto result = client->chat.completions.CreateStream(
    { .model = "gpt-5-mini", .messages = { moai::user_message("Hi") } },
    [&](moai::ChatCompletionChunk const& chunk) {
        accumulator.Add(chunk);
        for (auto const& choice : chunk.choices) {
            std::print("{}", choice.delta.content.value_or(""));
        }
        return true; // 返回 false 会停止流并得到 ErrorKind::Cancelled
    });
```

线程池异步返回 `std::future`，适合同时提交多个请求：

```cpp
auto first = client->chat.completions.CreateAsync(params);
auto second = client->chat.completions.CreateAsync(params);
if (first && second) {
    auto completion = first->get(); // 另一个 future 可稍后取回
}
```

协程接口返回 `mcr::Task`，需要 `import mcr;` 才能等待或取消：

```cpp
auto task = client->chat.completions.CreateCoro(params);
auto source = task.GetStopSource(); // 可从其他线程 request_stop()
auto completion = mcr::sync_wait(std::move(task));
```

`Conversation` 维护多轮历史，`parse_structured_output` 解析 JSON 输出并检查完成原因：

```cpp
moai::Conversation conversation;
conversation.Add(moai::system_message("Answer in one sentence."));
conversation.Add(moai::user_message("What is a C++ module?"));

auto completion = client->chat.completions.Create({
    .model = "gpt-5-mini",
    .messages = conversation.Messages(),
    .response_format = moai::ResponseFormatJsonSchema{
        .json_schema = { .name = "answer", .schema = moai::Json{ { "type", "object" } }, .strict = true },
    },
});
if (completion) {
    conversation.Add(*completion);
    if (auto parsed = moai::parse_structured_output(*completion)) {
        std::println("{}", parsed->dump());
    }
}
```

要点：

- **配置**：`ClientOptions` 可设置 `api_key`、`organization`、`project`、`base_url`、`timeout`
  （默认 600 s，限制整个传输）、`connect_timeout`（默认 5 s）、`max_retries`（默认 2）、
  默认请求头、默认查询参数和代理。未设置时读取 `OPENAI_API_KEY`、`OPENAI_BASE_URL`、
  `OPENAI_ORG_ID`、`OPENAI_PROJECT_ID`。
- **单次请求**：`RequestOptions` 可覆盖超时和重试次数，追加请求头、查询参数、`extra_body`，
  并通过 `std::stop_token` 取消传输或重试等待。
- **错误**：所有调用返回 `moai::Result<T>`（`std::expected<T, moai::Error>`），库本身不抛异常。
  `ErrorKind` 区分参数错误、请求准备失败、连接、超时、取消、各 HTTP 状态、解码和流错误；
  HTTP 错误保留状态码、`x-request-id` 及 API 的 `message` / `type` / `code` / `param`。
  流式回调抛出的异常会在传输停止后原样传出。
- **重试**：连接错误、超时、408、409、429 和 5xx 默认重试，遵循 `x-should-retry`、
  `retry-after-ms` 和 `retry-after`，否则按 0.5 s 起的指数退避；流式请求只在交付首个分块前重试。
- **线程**：`Client` 创建后配置不可变，可复制，可在多个线程中并发使用；每个请求使用独立的 mcr
  Session，并共享连接池。
- **异步与协程**：`CreateAsync` / `CreateStreamAsync` 在线程池工作线程上执行，回调也在该线程；
  `CreateCoro` / `CreateStreamCoro` 由 mcr 的 curl multi 运行时驱动，流式回调在 I/O 线程执行。
  协程路径的重试退避使用 `mcr::Coro::ScheduleAfter`，取消会立即中断等待。三种接口的错误和
  `std::stop_token` 取消语义一致。
- **curl 生命周期**：moai 不会关闭 libcurl。需要确定的退出清理时，可在 `main()` 开头创建
  `moai::GlobalInit` 守卫（或 `mcr::GlobalInit`），守卫必须比所有客户端、任务和进行中的请求
  存活更久；最后一个守卫销毁后会永久关闭协程和线程池运行时。
- **JSON**：自由结构字段（工具参数、JSON Schema、`extra_body`、`raw`）使用 `moai::Json`，
  即 `nlohmann::json`。花括号会构造数组，复制 Json 值时请用 `=` 或圆括号初始化。

完整示例见 [example/](example/)：`chat_basic`、`chat_stream`、`chat_tools`（工具调用循环）、
`chat_async`（并发线程池请求）、`chat_coro`（协程与确定清理）、`chat_structured_output`
（JSON Schema 结构化输出）。更详细的中文说明见 [docs/](docs/README.md)。

## 环境与依赖

需要 Git、mcpp，以及支持 C++23 模块和 `import std;` 的编译器与标准库。
安装方法见 [mcpp 安装说明](https://github.com/mcpp-community/mcpp#install)。
当前 [Windows CI](.github/workflows/windows-ci.yml) 和 [Linux CI](.github/workflows/linux-ci.yml)
固定使用 mcpp `2026.10.21.1`、LLVM `23.1.3`；Windows 还需要 Visual Studio 的 MSVC 工具和 Windows SDK。

必要时安装工具链；下面的 `default` 命令会更改本机 mcpp 的默认工具链：

```sh
mcpp toolchain install llvm 23.1.3
mcpp toolchain default llvm@23.1.3
mcpp self doctor
```

[mcpp.toml](mcpp.toml) 通过 Git URL 直接引入 mcr 的 `main` 分支，协程退避依赖该分支中的
`mcr::Coro::ScheduleAfter` / `ScheduleAt`；JSON 使用 mcpp 索引中的 nlohmann/json：

```toml
[dependencies]
mcr = { git = "https://github.com/yizhinailong/mcr.git", branch = "main" }

[dependencies.nlohmann]
json = "3.12.0"
```

这是 mcpp 的 [Git 依赖语法](https://github.com/mcpp-community/mcpp/blob/v2026.10.21.1/docs/05-dependencies.md)，
不需要预先克隆 mcr 或配置本地路径。首次构建会获取源码并记录解析结果到 `mcpp.lock`。
缓存齐备后可使用 `mcpp build --offline`。

| 依赖 | 版本 | 来源与职责 |
| --- | --- | --- |
| mcr | `main`（0.3.0 及以上） | 直接 Git 依赖；HTTP、连接池、SSE 解析、线程池与协程运行时 |
| nlohmann/json | `3.12.0` | 直接依赖，以 `import nlohmann.json;` 模块形式提供；请求序列化和响应解析 |
| curl | `8.21.0` | mcr 的传递依赖，由 mcpp 管理 |
| OpenSSL | `3.5.1` | mcr 在 Linux/macOS 上声明的传递依赖 |

平台 TLS 配置由 mcr 管理：Windows 使用 Schannel，Linux/macOS 启用 OpenSSL 上下文支持。
本仓库无需重复声明 curl、OpenSSL 或 `MCR_SSL_CTX_OPENSSL`。

## 构建与检查

```sh
git clone https://github.com/yizhinailong/moai.git
cd moai
mcpp build
mcpp test --timeout 90
```

测试使用 `tests/fixtures/mock_server.hpp` 中的本地回环 HTTP 服务返回预设响应，覆盖客户端配置、
请求序列化、响应解码、错误映射、已存储补全、SSE 流式、重试（含 HTTP 日期形式的 `retry-after`）、
超时和取消，同步 / 异步 / 协程三条路径的一致性和协程定时退避，以及对话历史、结构化输出和工具参数辅助。
测试不访问外部网络，不需要 OpenAI API key。根项目是库，没有可供 `mcpp run` 执行的默认程序。

生成编辑器编译数据库：

```sh
mcpp build --configure-only
```

Linux 上也可使用与 CI 相同的 ASan/UBSan 配置：

```sh
mcpp test --profile sanitize --timeout 90
```

示例是独立的 mcpp 项目，通过路径依赖引用 moai；运行时需要真实或兼容的服务：

```sh
cd example
mcpp build
mcpp run chat_basic -- gpt-5-mini
```

## 在其他项目中导入

在消费项目的 `mcpp.toml` 中引用本地 moai，路径相对于消费项目的配置文件：

```toml
[dependencies]
moai = { path = "../moai" }
```

之后即可按上文「快速开始」使用。`moai::VERSION`、`VERSION_MAJOR`、`VERSION_MINOR`、
`VERSION_PATCH` 和 `VERSION_NUM` 由 `build.mcpp` 从 `[package].version` 自动生成；
`VERSION_NUM` 为 `0xAABBCC` 格式。moai 的版本与依赖 mcr 的版本独立。

## 代码组织

```text
moai/
├── src/
│   ├── moai.cppm                   # 公开模块入口，重新导出公开分区
│   ├── client.cppm                 # moai:client，统一客户端
│   ├── core/
│   │   ├── config.cppm             # moai:config，ClientOptions、RequestOptions
│   │   ├── error.cppm              # moai:error，Error、ErrorKind、Result
│   │   ├── global_init.cppm        # moai:global_init，可选的进程级 curl / 运行时守卫
│   │   ├── json.cppm               # moai:json，Json 别名及内部解码辅助
│   │   ├── pagination.cppm         # moai:pagination，CursorPage
│   │   ├── transport.cppm / .cpp   # moai:transport（内部），mcr 同步/异步/协程请求、重试、错误映射、SSE
│   └── components/chat/
│       ├── types.cppm / .cpp       # moai:chat_types，请求/响应类型及序列化
│       ├── completions.cppm / .cpp # moai:chat_completions，Chat Completions 资源
│       ├── accumulator.cppm / .cpp # moai:chat_accumulator，流式分块合并
│       ├── conversation.cppm / .cpp # moai:chat_conversation，对话历史
│       └── helpers.cppm / .cpp     # moai:chat_helpers，结构化输出与工具参数
├── tests/
│   ├── fixtures/mock_server.hpp    # 本地回环 mock 服务与断言辅助
│   └── test_*.cpp                  # 独立测试程序
├── docs/                           # 中文使用文档与 Doxygen 说明
├── example/                        # 独立消费 moai 的示例项目
├── .github/workflows/              # 构建、测试、文档及发布配置
├── mcpp.toml                       # 包元数据、依赖和构建配置
├── build.mcpp                      # 生成 moai:version 分区
├── Doxyfile                        # API 文档配置
└── AGENTS.md                       # 开发约定
```

公共接口由 `src/moai.cppm` 统一导出，传输分区只在库内部使用。资源组件只处理参数、路径、
序列化和响应映射；认证、超时、重试、连接复用和流式解析位于核心层，底层 HTTP 由 mcr 执行。

## 文档与贡献

开发规范和验证命令见 [AGENTS.md](AGENTS.md)。使用文档采用简体中文，位于 [docs/](docs/README.md)：
配置与认证、错误处理、Chat Completions、流式/异步/协程、生命周期与线程，以及 Doxygen 构建说明。
新增接口应同步补充测试、文档和状态说明。HTTP 测试应使用本地服务或固定响应，避免依赖真实密钥和付费请求。

安装 Doxygen 后，先创建 `target/doxygen` 目录，再在仓库根目录运行：

```sh
doxygen Doxyfile
```

生成首页位于 `target/doxygen/html/index.html`。[Documentation 工作流](.github/workflows/docs.yml)
配置为在 `main` 上的相关更新后构建并部署 GitHub Pages；仓库需将 Pages 来源设置为 GitHub Actions。
[版本标签工作流](.github/workflows/version-tag.yml) 在 `main` 的包版本变化时创建 `v<version>` 标签并触发发布检查，
[Release 工作流](.github/workflows/release.yml) 在 Windows/Linux 检查通过后发布 `moai-*` 包。

## 许可证

采用 [MIT 许可证](LICENSE)。
