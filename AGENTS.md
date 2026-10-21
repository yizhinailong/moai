# moai 开发指南

## 项目定位

moai 是基于 C++23 模块的非官方 OpenAI 客户端库，使用 mcpp 构建，以
[mcr](https://github.com/yizhinailong/mcr)（`main` 分支，0.3.0 及以上）为 HTTP 传输层。
公开入口为 `import moai;`，命名空间为 `moai`。

- 代码组织参考 [liboai](https://github.com/D7EAD/liboai) 的统一入口、`core` 和 `components` 分层。
- API 功能、参数及行为以 [OpenAI 官方 API](https://developers.openai.com/api/reference/overview)
  和 [官方 Python SDK](https://developers.openai.com/api/reference/python) 为依据。
  liboai 已归档，其历史接口列表不代表当前 OpenAI API。
- 已实现核心层和 Chat Completions 的同步调用、SSE 流式、线程池异步、协程、已存储补全管理，
  以及对话历史、结构化输出和工具参数辅助；其他资源尚未实现。新增功能后同步更新 README 的状态。
- 首个里程碑是 Chat Completions。开发计划和待办记录在仓库根目录的 `TODO.md`
  （本地文件，被 `.gitignore` 忽略，不提交）；完成任务或调整计划时同步更新它。
- 使用简体中文编写使用文档；API 名称、命令和程序输出保留原文。

## 构建与依赖

- 唯一项目构建入口是 `mcpp.toml`，不另建 CMake 或 xmake 配置。
- 当前 CI 固定 mcpp `2026.10.21.1`、LLVM `23.1.3`，覆盖 Windows 和 Linux。
  Windows 还需要 MSVC 工具及 Windows SDK；Linux 的 `sanitize` 配置启用 ASan/UBSan。
- mcr 使用 Git URL 声明 `main` 分支；协程重试退避依赖该分支的
  `mcr::Coro::ScheduleAfter` / `ScheduleAt`，发布前再改为固定标签：

  ```toml
  [dependencies]
  mcr = { git = "https://github.com/yizhinailong/mcr.git", branch = "main" }
  ```

- 不将 mcr 源码复制进本仓库，也不提交依赖于开发者本机目录的 mcr `path` 配置。
  curl、平台 OpenSSL 依赖和 `MCR_SSL_CTX_OPENSSL` 定义由 mcr 自己管理。
- JSON 使用 mcpp 索引的 `nlohmann.json` 3.12.0（`[dependencies.nlohmann] json = "3.12.0"`，
  `import nlohmann.json;`），公开别名为 `moai::Json`，只用于自由结构字段；已知字段使用强类型结构体。
- `build.mcpp` 从 `[package].version` 生成 `moai:version`。不要手工维护第二份版本常量，
  不要修改 `target/` 中的生成文件或 mcpp 的依赖缓存。
- 项目版本和 mcr 版本独立；发布脚本从 `mcpp.toml` 读取 moai 版本。

## 源码组织

当前文件：

- `src/moai.cppm`：公开模块总入口，重新导出公开分区。
- `src/client.cppm`（`moai:client`）：统一客户端 `moai::Client`，挂载各资源组件。
- `src/core/`：`config`（`ClientOptions`、`RequestOptions`）、`error`（`Error`、`ErrorKind`、`Result`）、
  `global_init`（可选的进程级 `mcr::GlobalInit` 包装）、`json`（`Json` 别名及内部 `JsonReader`）、
  `pagination`（`CursorPage`），以及内部分区 `transport`：`transport.cppm` 声明、`transport.cpp`
  实现 mcr 同步 / 异步 / 协程请求、重试、错误映射和 SSE，不由 `moai.cppm` 重新导出。
- `src/components/chat/`：`types`（请求/响应类型，序列化在 `types.cpp`）、`completions`（资源接口）、
  `accumulator`（流式分块合并）、`conversation`（对话历史）、`helpers`（结构化输出与工具参数）。
- `build.mcpp`：生成 `moai:version`，提供 `moai::VERSION` 等常量。
- `tests/fixtures/mock_server.hpp`：本地回环 mock 服务（脚本化响应、SSE 分块、延迟、断连）和断言辅助。
- `tests/test_*.cpp`：依赖集成、客户端配置、Create、已存储补全、流式、重试/超时/取消、
  线程池异步和协程路径，以及对话历史、结构化输出和工具参数辅助。
- `example/`：独立消费 moai 的 mcpp 项目（`chat_basic`、`chat_stream`、`chat_tools`、
  `chat_async`、`chat_coro`、`chat_structured_output`）。
- `docs/`：简体中文使用文档和 Doxygen 构建说明；CI 同时构建 `example/`。

后续按实际实现增加目录和分区，不提前堆积空文件：新资源放在 `src/components/<resource>/`，
在 `Client` 上增加对应成员。

资源组件只处理该资源的参数、路径、序列化和响应映射。认证、超时、重试和连接复用放在核心层，
底层 HTTP、TLS、代理、Multipart、异步和 SSE 尽量复用 mcr。

## C++ 约定

- 使用 C++23 和 `import std;`。模块接口使用 `.cppm` 文件，需要隐藏的实现放在 `.cpp` 文件。
  公开分区通过 `src/moai.cppm` 导出；不通过 moai 总入口重新导出整个 mcr。
- 按现有 `.clang-format` 格式化，不修改该文件。
- 类型使用 `PascalCase`；对象、参数使用 `snake_case`；公开成员函数使用 `PascalCase`；
  私有成员函数使用 `camelCase`；自由函数使用 `snake_case`；私有数据成员使用 `m_` 前缀；
  常量使用 `UPPER_SNAKE_CASE`。
- 使用花括号初始化、`T const&`、只读字符串参数 `std::string_view` 和 RAII。
  可恢复错误优先采用 `std::expected`；避免裸 `new` / `delete`。
- 例外：`moai::Json`（nlohmann::json）的花括号是初始化列表，`Json value{ other }` 会得到单元素数组。
  复制或转换 Json 值时使用 `=` 或圆括号；只有书写对象/数组字面量时才用花括号。
- 公开聚合类型（参数、响应结构体）的每个字段都写默认成员初始化（如 `std::string name{};`）。
  否则 `-Wextra` 下省略字段的指定初始化器会触发 `-Wmissing-designated-field-initializers`，
  库和使用者开启 `-Werror` 时均会编译失败。
- 解码器内部可抛出 `detail::DecodeFailure`，但必须在 API 边界转换为 `ErrorKind::Decode`；
  公开接口不向调用方抛出库异常，只有用户回调抛出的异常按原样传出。
- 为公开接口和文件添加简洁的 Doxygen 注释，说明所有权、生命周期及错误语义。

## API 实现约束

- 先实现 Chat Completions 所需的核心层（配置/认证、错误映射、JSON、传输、重试），再实现
  Chat Completions 的创建、SSE 流式、异步/协程及已存储补全的管理接口；之后扩展 Models、Responses
  和其他资源。开发具体端点前核对官方文档，不直接照搬 liboai 的旧 Completions、Edits、Fine-tunes
  接口或已弃用的 `functions` / `function_call` 参数。
- 明确区分 mcr 的请求准备失败、`Response::error` 传输失败，以及 HTTP/API 错误。
  mcr 的 HTTP 4xx/5xx 是普通响应；moai 需要映射成 SDK 错误并保留状态码和请求 ID。
- API key 从配置或 `OPENAI_API_KEY` 获取；允许配置 base URL 以便本地测试。
  示例和日志不得包含真实密钥。
- 同步、future/协程和 SSE 接口保持一致的错误与取消语义。一个 Session 的配置和请求需串行使用。
  mcr 异步任务和借用的请求数据必须有明确的生命周期；进程级运行时不能随每次请求关闭。
  `Client` 也不能持有 `mcr::GlobalInit`：最后一个守卫的清理是永久的，之后无法再创建客户端。
- 流式请求不直接使用 mcr 的 `Session::SetServerSentEventCallback`：设置后响应体只交给 SSE 解析器，
  不写入 `Response::text`，非 SSE 的错误响应体会丢失。应通过 `HeaderCallback` / `WriteCallback`
  判断状态码和 `Content-Type`，成功时再驱动 `mcr::ServerSentEventParser`，失败时保留错误响应体。
- 重试要考虑请求体能否重放及流式输出是否已经交付；分页、Multipart、流式事件需要各自的行为测试。
  Realtime 的 WebSocket 会话需单独设计，不能把 SSE 支持等同于 Realtime 支持。

## 验证与维护

```sh
mcpp build
mcpp test --timeout 90
mcpp build --configure-only
```

测试采用 `tests/test_*.cpp` 独立程序。HTTP/API 测试使用 `tests/fixtures/mock_server.hpp`
提供的本地服务和固定响应，不依赖真实 API key、付费请求或外部网络。修改实际行为时补充有针对性的测试。
Linux 可运行 `mcpp test --profile sanitize --timeout 90`。修改公开接口后在 `example/` 中运行
`mcpp build`，确认独立消费项目仍能编译。

文档使用 `doxygen Doxyfile` 构建，输出在 `target/doxygen/html/`；先创建 `target/doxygen`。
新增文档目录后更新 Doxyfile 的 `INPUT` 和相应 CI 路径。
修改构建、版本或目录时，同时检查 `.github/workflows/` 中的引用和产物名。
提交前运行 `git diff --check`，不要把构建产物、密钥或未验证的功能声明写入提交。
