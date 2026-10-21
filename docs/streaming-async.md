# 流式、异步与协程

Chat Completions 提供三种调用方式，错误和取消语义一致：

| 方式 | 接口 | 返回 | 回调线程 |
| --- | --- | --- | --- |
| 同步 | `Create` / `CreateStream` | `Result<T>` | 调用线程 |
| 线程池异步 | `CreateAsync` / `CreateStreamAsync` | `Result<std::future<Result<T>>>` | 线程池工作线程 |
| 协程 | `CreateCoro` / `CreateStreamCoro` | `mcr::Task<Result<T>>` | mcr 的 I/O 线程 |

## SSE 流式

```cpp
moai::ChatCompletionAccumulator accumulator;
auto result = client->chat.completions.CreateStream(
    {
        .model = "gpt-5-mini",
        .messages = { moai::user_message("写一首两行的诗") },
        .stream_options = moai::ChatCompletionStreamOptions{ .include_usage = true },
    },
    [&](moai::ChatCompletionChunk const& chunk) {
        accumulator.Add(chunk);
        for (auto const& choice : chunk.choices) {
            std::print("{}", choice.delta.content.value_or(""));
        }
        return true;
    }
);
if (!result) {
    std::println(std::cerr, "\n{}", result.error());
}
auto completion = accumulator.Take();
```

要点：

- `CreateStream` 自动写入 `"stream": true`，不需要手动设置。
- 回调返回 `false` 会停止传输并返回 `Cancelled`；`accumulator` 保留已收到的内容。
- 回调在运行传输的线程上调用，`chunk` 只在调用期间有效，需要保留时请复制或使用累加器。
- 回调抛出的异常会在传输停止后原样传出，不转换为 `Error`。
- 空回调返回 `InvalidArgument`，不会发送请求。
- 请求只在交付首个分块前重试；一旦有分块交付，后续失败直接返回错误。
- 非 2xx 响应的错误体会保留在 `Error::body`；2xx 但 `Content-Type` 不是 `text/event-stream`
  时返回 `Stream`。

`ChatCompletionAccumulator` 按 choice 拼接 content/refusal，按 `tool_calls[].index` 合并工具调用，
记录 `finish_reason`，追加 logprobs，并取最后一个报告 usage 的分块。

## 线程池异步

```cpp
auto first = client->chat.completions.CreateAsync(params);
auto second = client->chat.completions.CreateAsync(params);
if (!first || !second) {
    // 提交失败：线程池已清理或任务无法提交
}
auto a = first->get();
auto b = second->get();
```

- 参数和客户端共享状态都复制进任务，`Client` 可以先销毁。
- 返回值有两层错误：外层是提交失败（`RequestSetup`），内层是请求结果。
- `future` 必须用 `get()` 消费；丢弃 `future` 不会取消任务，任务仍会在后台运行。
- 流式的回调在线程池工作线程执行，同样不得阻塞。

取消通过 `RequestOptions::stop_token`：

```cpp
std::stop_source source;
auto future = client->chat.completions.CreateAsync(params, { .stop_token = source.get_token() });
source.request_stop();
auto result = future->get(); // ErrorKind::Cancelled
```

## 协程

协程接口返回 `mcr::Task`，需要 `import mcr;` 才能等待或操作任务：

```cpp
import mcr;
import moai;

auto task = client->chat.completions.CreateCoro(params);
auto source = task.GetStopSource();          // 可从其他线程 request_stop()
auto completion = mcr::sync_wait(std::move(task));
```

- `Task::Cancel()`、父任务取消和 `RequestOptions::stop_token` 都映射为 `Cancelled`。
- 任务拥有参数和客户端共享状态，可以比创建它的 `Client` 存活更久。
- 重试退避使用 `mcr::Coro::ScheduleAfter`，不阻塞线程；取消会立即结束等待。
- 流式回调在 mcr curl multi 的 I/O 线程执行，必须快速返回；不要在回调中等待另一个协程。
- 协程运行时必须处于运行状态。程序退出前可用 `mcr::Coro::Cleanup()` 关闭，或使用
  `moai::GlobalInit` 守卫，见[生命周期与线程](lifecycle.md)。

在协程中调用：

```cpp
auto summarize(moai::Client& client, std::string text) -> mcr::Task<moai::Result<std::string>> {
    auto completion = co_await client.chat.completions.CreateCoro({
        .model = "gpt-5-mini",
        .messages = { moai::user_message(std::move(text)) },
    });
    if (!completion) {
        co_return std::unexpected{ std::move(completion.error()) };
    }
    co_return completion->choices.at(0).message.content.value_or("");
}
```

## 取消语义对照

| 操作 | 同步 | 异步 | 协程 |
| --- | --- | --- | --- |
| `RequestOptions::stop_token` | 中断传输和重试等待 | 同左 | 同左 |
| 任务自身的 `Cancel()` | 不适用 | 不适用（使用 `stop_token`） | 中断传输和重试等待 |
| 流回调返回 `false` | `Cancelled` | `Cancelled` | `Cancelled` |
| 运行时清理 | 不影响已开始的调用 | 等待中的任务失败 | 运行中的任务失败 |

所有路径都返回 `ErrorKind::Cancelled`，不会抛出取消异常。
