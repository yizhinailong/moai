# moai 开发指南

## 项目定位

moai 是基于 C++23 模块的非官方 OpenAI 客户端库，使用 mcpp 构建，以
[mcr v0.3.0](https://github.com/yizhinailong/mcr/tree/v0.3.0) 为 HTTP 传输层。
公开入口为 `import moai;`，命名空间为 `moai`。

- 代码组织参考 [liboai](https://github.com/D7EAD/liboai) 的统一入口、`core` 和 `components` 分层。
- API 功能、参数及行为以 [OpenAI 官方 API](https://developers.openai.com/api/reference/overview)
  和 [官方 Python SDK](https://developers.openai.com/api/reference/python) 为依据。
  liboai 已归档，其历史接口列表不代表当前 OpenAI API。
- 当前是初始化骨架：只有模块入口、自动生成的版本信息和依赖集成测试。
  OpenAI 客户端、资源接口、JSON、流式解析尚未实现。新增功能后同步更新 README 的状态。
- 使用简体中文编写使用文档；API 名称、命令和程序输出保留原文。

## 构建与依赖

- 唯一项目构建入口是 `mcpp.toml`，不另建 CMake 或 xmake 配置。
- 当前 CI 固定 mcpp `2026.10.21.1`、LLVM `23.1.3`，覆盖 Windows 和 Linux。
  Windows 还需要 MSVC 工具及 Windows SDK；Linux 的 `sanitize` 配置启用 ASan/UBSan。
- mcr 必须使用 Git URL 和固定标签声明：

  ```toml
  [dependencies]
  mcr = { git = "https://github.com/yizhinailong/mcr.git", tag = "v0.3.0" }
  ```

- 不将 mcr 源码复制进本仓库，也不提交依赖于开发者本机目录的 mcr `path` 配置。
  curl、平台 OpenSSL 依赖和 `MCR_SSL_CTX_OPENSSL` 定义由 mcr 自己管理。
- `build.mcpp` 从 `[package].version` 生成 `moai:version`。不要手工维护第二份版本常量，
  不要修改 `target/` 中的生成文件或 mcpp 的依赖缓存。
- 项目版本和 mcr 版本独立；发布脚本从 `mcpp.toml` 读取 moai 版本。

## 源码组织

当前文件：

- `src/moai.cppm`：公开模块总入口，重新导出公开分区。
- `build.mcpp`：生成 `moai:version`，提供 `moai::VERSION` 等常量。
- `tests/test_dependency.cpp`：验证模块导入、mcr/curl 链接和会话初始化，不发网络请求。

后续按实际实现增加目录和分区，不提前堆积空文件：

- `src/core/`：配置、认证、JSON 适配、传输封装、错误、分页和流式基础设施。
- `src/components/`：Responses、Chat Completions、Models 等资源组件。
- `src/client.cppm`：组合配置、传输及资源组件的统一客户端。
- `example/`：独立消费 moai 的可运行示例；`docs/`：详细中文文档。

资源组件只处理该资源的参数、路径、序列化和响应映射。认证、超时、重试和连接复用放在核心层，
底层 HTTP、TLS、代理、Multipart、异步和 SSE 尽量复用 mcr。
JSON 依赖应在实现序列化时明确选择；mcr 本身只处理字节，不提供 JSON 对象模型。

## C++ 约定

- 使用 C++23 和 `import std;`。模块接口使用 `.cppm` 文件，需要隐藏的实现放在 `.cpp` 文件。
  公开分区通过 `src/moai.cppm` 导出；不通过 moai 总入口重新导出整个 mcr。
- 按现有 `.clang-format` 格式化，不修改该文件。
- 类型使用 `PascalCase`；对象、参数使用 `snake_case`；公开成员函数使用 `PascalCase`；
  私有成员函数使用 `camelCase`；自由函数使用 `snake_case`；私有数据成员使用 `m_` 前缀；
  常量使用 `UPPER_SNAKE_CASE`。
- 使用花括号初始化、`T const&`、只读字符串参数 `std::string_view` 和 RAII。
  可恢复错误优先采用 `std::expected`；避免裸 `new` / `delete`。
- 为公开接口和文件添加简洁的 Doxygen 注释，说明所有权、生命周期及错误语义。

## API 实现约束

- 优先实现配置/认证、错误映射、Models、Responses、Chat Completions，再扩展其他资源。
  开发具体端点前核对官方文档，不直接照搬 liboai 的旧 Completions、Edits 或 Fine-tunes 接口。
- 明确区分 mcr 的请求准备失败、`Response::error` 传输失败，以及 HTTP/API 错误。
  mcr 的 HTTP 4xx/5xx 是普通响应；moai 需要映射成 SDK 错误并保留状态码和请求 ID。
- API key 从配置或 `OPENAI_API_KEY` 获取；允许配置 base URL 以便本地测试。
  示例和日志不得包含真实密钥。
- 同步、future/协程和 SSE 接口保持一致的错误与取消语义。一个 Session 的配置和请求需串行使用。
  mcr 异步任务和借用的请求数据必须有明确的生命周期；进程级运行时不能随每次请求关闭。
- 重试要考虑请求体能否重放及流式输出是否已经交付；分页、Multipart、流式事件需要各自的行为测试。
  Realtime 的 WebSocket 会话需单独设计，不能把 SSE 支持等同于 Realtime 支持。

## 验证与维护

```sh
mcpp build
mcpp test --timeout 90
mcpp build --configure-only
```

测试采用 `tests/test_*.cpp` 独立程序。未来 HTTP/API 测试使用本地服务或固定响应，
不依赖真实 API key、付费请求或外部网络。修改实际行为时补充有针对性的测试。
Linux 可运行 `mcpp test --profile sanitize --timeout 90`。

文档使用 `doxygen Doxyfile` 构建，输出在 `target/doxygen/html/`；先创建 `target/doxygen`。
新增文档目录后更新 Doxyfile 的 `INPUT` 和相应 CI 路径。
修改构建、版本或目录时，同时检查 `.github/workflows/` 中的引用和产物名。
提交前运行 `git diff --check`，不要把构建产物、密钥或未验证的功能声明写入提交。
