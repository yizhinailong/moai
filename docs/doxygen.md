# API 文档构建

仓库使用 Doxygen 从源码注释和 Markdown 文档生成 HTML 文档。配置为根目录的
[Doxyfile](../Doxyfile)，输入包括 `src/`、`docs/`、`README.md` 和 `AGENTS.md`。

## 本地构建

1. 安装 Doxygen 1.18.0 或更新版本。
2. 在仓库根目录创建输出目录并运行：

```sh
mkdir -p target/doxygen
doxygen Doxyfile
```

3. 打开 `target/doxygen/html/index.html`。

Windows PowerShell 等价命令：

```powershell
New-Item -ItemType Directory -Force target/doxygen | Out-Null
doxygen Doxyfile
```

`target/` 是构建产物目录，不提交到仓库。修改 `Doxyfile` 的 `INPUT` 或文档目录后，同时确认
[Documentation 工作流](../.github/workflows/docs.yml) 的触发路径仍然覆盖改动。

## CI 与发布

[Documentation 工作流](../.github/workflows/docs.yml) 在 `main` 分支的相关文件变化后：

1. 下载固定版本的 Doxygen；
2. 运行 `doxygen Doxyfile` 并检查 `index.html` 存在；
3. 将 `target/doxygen/html` 作为 GitHub Pages 产物上传并部署。

仓库需要在 GitHub 的 Pages 设置中选择 GitHub Actions 作为来源。

## 注释约定

- 公开类型、函数和分区文件使用 `@brief` 简短说明，并通过 `@param` / `@return` / `@note`
  描述所有权、生命周期和错误语义。
- 请求类型字段使用行尾注释；每个公开聚合字段都写默认成员初始化。
- 示例代码使用 `@code` / `@endcode`，确保与当前接口一致。
- 中文说明放在 Markdown 文档中；Doxygen 注释可以使用简洁英文或中文，但同一文件内保持一致。
