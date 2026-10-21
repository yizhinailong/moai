# moai 使用文档

moai 是基于 C++23 模块的 OpenAI 客户端库，公开入口为 `import moai;`。当前版本实现了
Chat Completions 的同步、SSE 流式、线程池异步、协程、工具调用、结构化输出、已存储补全管理，
以及对话历史辅助 `Conversation`。其他资源尚未实现，状态以仓库根目录的
[README](../README.md#当前状态) 为准。

## 安装与构建

在消费项目的 `mcpp.toml` 中引用本地 moai：

```toml
[dependencies]
moai = { path = "../moai" }
```

随后即可在源码中导入：

```cpp
import std;
import moai;
```

完整的环境要求、依赖版本和测试命令见 [README](../README.md#环境与依赖)。示例项目位于
[example/](../example/)，通过路径依赖引用 moai。

## 文档索引

| 文档 | 内容 |
| --- | --- |
| [配置与认证](configuration.md) | `ClientOptions`、`RequestOptions`、环境变量、代理和请求头 |
| [错误处理](errors.md) | `Result`、`ErrorKind`、重试策略和错误内容 |
| [Chat Completions](chat-completions.md) | 消息、参数、工具调用、结构化输出和已存储补全 |
| [流式、异步与协程](streaming-async.md) | SSE 回调、线程池 future、`mcr::Task` 和取消 |
| [生命周期与线程](lifecycle.md) | 客户端共享状态、回调线程、运行时清理 |

## 最小示例

```cpp
import std;
import moai;

auto main() -> int {
    auto guard = moai::GlobalInit::Create(); // 可选的进程级清理守卫
    auto client = moai::Client::Create();    // 读取 OPENAI_API_KEY
    if (!client) {
        std::println("{}", client.error());
        return 1;
    }
    auto completion = client->chat.completions.Create({
        .model = "gpt-5-mini",
        .messages = { moai::user_message("用一句话解释 C++ 模块") },
    });
    if (!completion) {
        std::println("{}", completion.error());
        return 1;
    }
    std::println("{}", completion->choices.at(0).message.content.value_or(""));
    return 0;
}
```
