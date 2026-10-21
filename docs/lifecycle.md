# 生命周期与线程

## 客户端

`moai::Client` 只是共享状态的句柄：

- 创建后配置不可变。
- 可以复制和移动；所有副本共享同一份配置和 `mcr::ConnectionPool`。
- 可以在多个线程中并发调用；每次请求使用独立的 `mcr::Session`，连接池内部同步。
- 资源的成员（如 `client.chat.completions`）与客户端绑定，不要跨客户端混用。

```cpp
auto client = moai::Client::Create({ .api_key = "sk-..." });
auto copy = *client; // 共享共享状态
```

异步和协程任务会持有自己的 `std::shared_ptr<ClientCore const>`，因此 `Client` 先销毁不会
使进行中的请求失效。

## 请求数据

- 同步接口的参数只在调用期间借用，函数返回后不再引用调用方对象。
- 异步和协程接口按值复制参数与 `RequestOptions`；调用返回后可以立即修改或销毁原对象。
- `RequestOptions::stop_token` 复制的是 token，停止源由调用方保留。
- 所有错误和结果都拥有自己的字符串，不引用响应缓冲区。

## 回调

| 场景 | 回调线程 | 生命周期 |
| --- | --- | --- |
| `CreateStream` | 调用 `CreateStream` 的线程 | `chunk` 只在调用期间有效 |
| `CreateStreamAsync` | 线程池工作线程 | 同上 |
| `CreateStreamCoro` | mcr curl multi 的 I/O 线程 | 同上 |

回调不得阻塞。在协程路径中阻塞 I/O 线程会延迟所有其他协程。回调返回 `false` 表示正常停止；
抛出的异常会停止传输并按原样传给调用方。

`ChatCompletionAccumulator`、`Conversation` 等辅助类型不是线程安全的。每个流或每个线程使用
独立实例，或由调用方加锁。

## mcr 运行时与清理

moai 不自行关闭 libcurl，也不在每次请求后清理运行时。需要确定的退出清理时，在 `main()` 开头
创建守卫：

```cpp
auto guard = moai::GlobalInit::Create(); // 包装 mcr::GlobalInit
if (!guard) {
    std::println(std::cerr, "{}", guard.error());
    return 1;
}
```

`moai::GlobalInit` 是 RAII 守卫，内部持有 `mcr::GlobalInit`：

- 必须比所有客户端、任务和进行中的请求存活更久。
- 最后一个守卫销毁后，curl、协程运行时和线程池被**永久**关闭，之后无法再创建客户端或提交任务。
- 因此不要把它放进 `Client`，也不要在库内部创建短生命周期守卫。
- 不需要确定性清理时可以完全不创建守卫，进程退出时由操作系统回收。

只使用协程且希望显式关闭运行时，也可以在等待所有任务后调用 `mcr::Coro::Cleanup()`。
`Cleanup()` 之后再提交协程任务会失败。

## 资源释放顺序

推荐的顺序是：

```text
GlobalInit 创建
  → Client 创建
    → 请求、任务、流
  ← 所有任务与流结束
  ← Client 销毁
GlobalInit 销毁
```

示例 [chat_coro](../example/src/chat_coro.cpp) 演示了该顺序。

## 分页与已存储补全

`CursorPage<T>` 拥有当页数据，不引用客户端状态。自动翻页需要调用方使用 `last_id` 继续请求；
`HasNextPage()` 等价于 `has_more`。
