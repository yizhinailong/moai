# moai

基于 [mcr 0.3.0](https://github.com/yizhinailong/mcr/tree/v0.3.0) 的 C++23 模块化 OpenAI 客户端库，
使用 [mcpp](https://github.com/mcpp-community/mcpp) 构建和管理依赖。

代码组织参考 [liboai](https://github.com/D7EAD/liboai) 的统一客户端、核心层和资源组件，
功能设计对齐 [OpenAI 官方 SDK](https://developers.openai.com/api/reference/python)
和 [API 文档](https://developers.openai.com/api/reference/overview)。这是非官方 C++ 实现。

## 当前状态

当前版本为 `0.1.0`，处于项目初始化阶段。已具备 `import moai;` 入口、自动生成的版本信息、
mcr 的固定版本 URL 依赖、依赖集成测试，以及 Windows/Linux CI 和 Doxygen 配置。
**OpenAI 请求接口尚未实现**；目前不能通过 moai 调用模型。

liboai 已归档，本项目借鉴其代码分层，具体端点和行为以 OpenAI 官方资料为准。
以下是后续开发顺序，并非已支持的功能清单：

| 阶段 | 计划内容 | 状态 |
| --- | --- | --- |
| 核心层 | 客户端配置、API key、base URL、JSON、错误映射、超时和重试 | 待实现 |
| 首批资源 | Models、Responses、Chat Completions | 待实现 |
| 调用方式 | 同步、future/协程异步、SSE 流式事件、取消 | 待实现 |
| 常用资源 | Embeddings、Images、Audio、Files、Uploads、Moderations | 待实现 |
| 扩展资源 | Conversations、Batches、Fine-tuning、Vector Stores、分页辅助 | 待实现 |
| 后续评估 | Realtime/WebSocket、Webhooks 及其他官方资源 | 待设计 |

Responses 是新功能的优先入口，同时规划 Chat Completions 以支持现有应用。
工具调用、结构化输出和多模态参数随相应资源逐步实现，具体字段以官方 API 为准。

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

[mcpp.toml](mcpp.toml) 通过 Git URL 直接引入 mcr，并固定到 `v0.3.0`：

```toml
[dependencies]
mcr = { git = "https://github.com/yizhinailong/mcr.git", tag = "v0.3.0" }
```

这是 mcpp 的 [Git 依赖语法](https://github.com/mcpp-community/mcpp/blob/v2026.10.21.1/docs/05-dependencies.md)，
不需要预先克隆 mcr 或配置本地路径。首次构建会获取源码并记录解析结果到 `mcpp.lock`。
缓存齐备后可使用 `mcpp build --offline`。

| 依赖 | 版本 | 来源与职责 |
| --- | --- | --- |
| mcr | `0.3.0` | 直接 Git 依赖；HTTP、异步、Multipart、SSE 等传输能力 |
| curl | `8.21.0` | mcr 的传递依赖，由 mcpp 管理 |
| OpenSSL | `3.5.1` | mcr 在 Linux/macOS 上声明的传递依赖 |

平台 TLS 配置由 mcr 管理：Windows 使用 Schannel，Linux/macOS 启用 OpenSSL 上下文支持。
本仓库无需重复声明 curl、OpenSSL 或 `MCR_SSL_CTX_OPENSSL`。mcr 不包含 JSON 库，
moai 的 JSON 适配会在实现核心层时引入。

## 构建与检查

```sh
git clone https://github.com/yizhinailong/moai.git
cd moai
mcpp build
mcpp test --timeout 90
```

测试会导入 moai 和 mcr，创建 curl 初始化守卫及 HTTP Session，验证传递依赖能正确链接和初始化。
测试不会发送网络请求，不需要 OpenAI API key。根项目是库，没有可供 `mcpp run` 执行的默认程序。

生成编辑器编译数据库：

```sh
mcpp build --configure-only
```

Linux 上也可使用与 CI 相同的 ASan/UBSan 配置：

```sh
mcpp test --profile sanitize --timeout 90
```

## 在其他项目中导入

初始化阶段可在消费项目的 `mcpp.toml` 中引用本地 moai，路径相对于消费项目的配置文件：

```toml
[dependencies]
moai = { path = "../moai" }
```

在消费项目的 `src/main.cpp` 中读取当前已提供的版本信息，然后运行 `mcpp run`：

```cpp
import std;
import moai;

auto main() -> int {
    std::println("moai {}", moai::VERSION);
}
```

`moai::VERSION_MAJOR`、`VERSION_MINOR`、`VERSION_PATCH` 和 `VERSION_NUM` 也可使用。
版本常量由 `build.mcpp` 从 `[package].version` 自动生成；`VERSION_NUM` 为 `0xAABBCC` 格式。
moai 的版本与依赖 mcr 的版本独立。

## 代码组织

当前已存在的主要文件：

```text
moai/
├── src/
│   └── moai.cppm             # 公开模块入口
├── tests/
│   └── test_dependency.cpp   # mcr/curl 依赖集成检查
├── .github/workflows/       # 构建、测试、文档及发布配置
├── mcpp.toml                # 包元数据、依赖和构建配置
├── build.mcpp               # 生成 moai:version 分区
├── Doxyfile                 # API 文档配置
└── AGENTS.md                # 开发约定
```

后续实现按下面的结构逐步增加，以下路径当前尚未创建：

| 路径 | 职责 |
| --- | --- |
| `src/client.cppm` | 统一客户端，组合配置、传输和各资源组件 |
| `src/core/` | 认证、配置、JSON、传输封装、错误、分页、流式基础设施 |
| `src/components/` | 按官方 API 资源拆分的组件，如 Responses、Chat、Models |
| `example/` | 独立消费 moai 的可运行示例 |
| `docs/` | 中文使用文档与设计说明 |

公共接口由 `src/moai.cppm` 统一导出。资源组件共享核心层，通过 mcr 执行 HTTP 请求。
SDK 层负责序列化、资源语义和错误映射，包括区分请求准备失败、传输失败及 HTTP/API 错误。

## 文档与贡献

开发规范和验证命令见 [AGENTS.md](AGENTS.md)。使用文档采用简体中文；新增接口应同步补充测试和状态说明。
HTTP 测试应使用本地服务或固定响应，避免依赖真实密钥和付费请求。

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
