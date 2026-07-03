# BUA Runtime API 与 Bridge 接口设计计划

## Core Purpose
- 面向 LLM 提供一套稳定、结构化、容易理解的 BUA tool call 接口，让 agent 可以直接感知和操作浏览器页面。
- 将面向 LLM 的工具协议和底层浏览器能力解耦：Bridge 负责工具定义、参数校验、上下文管理和结果格式化；新的 BUA API 负责封装可复用的浏览器操作能力。
- 以新的 BUA API 作为后续替代旧 `bua_api.ts` 的契约基础，支撑未来接入 Glic C++ backend 或其他 backend。

## Core Constraints
- LLM 看到的工具必须稳定、明确、低歧义，参数和返回值都要适合模型直接理解和继续推理。
- Bridge 必须隐藏不必要的运行时细节，让 LLM 专注于页面感知、页面操作和用户接管。
- Bridge 和新的 BUA API 必须分层清晰：Bridge 只做工具协议和 LLM 友好格式转换，新的 BUA API 只做浏览器能力封装。
- 页面感知和操作结果必须能指导下一步动作；Bridge 面向 LLM 返回失败时要给出可理解的原因。

## Summary
- 新增 `components/bua/bua_api_bridge.ts`：面向 LLM tool call 的 Bridge 接口，使用通用 JSON Schema 描述工具，隐藏 session/task 细节。
- 新增 `components/bua/bua_api2.ts`：作为未来替代 `bua_api.ts` 的新 JS API 契约，专门支撑 Bridge 和后续 runtime；导出的 API 类型不使用 `2` 后缀。
- 本轮只做接口与契约，不写具体 runtime 实现，不修改旧 `bua_api.ts`。
- 只覆盖文档中的核心能力；不写低优能力 `page_search/eval/mouse/shortcut/scripttool`。

## Key Changes
- `bua_api_bridge.ts` 定义：
  - `BuaBridgeToolName` 只包含核心工具：
    `bua_tab_new/list/current/activate/close`，
    `bua_page_navigate/back/forward/reload`，
    `bua_page_snapshot/screenshot/extract_content`，
    `bua_page_click/type/scroll/scrollto/movemouse/drag/select`，
    `bua_page_wait`，
    `bua_take_over`。
  - `BuaBridgeToolDefinition` 使用中立 JSON Schema：`name / description / inputSchema / outputMode`。
  - `BuaBridge` 初始化时由外部 host 提供 `BuaSession`，并暴露 `tools()` 和 `invokeTool(name, args, context?)`。
  - `invokeTool()` 是统一执行入口：校验 LLM tool call 参数，使用初始化时注入的 session 和当前 tab 上下文，调用新的 BUA API，并格式化返回。
  - `BuaBridgeResult` 使用 `ok / data? / content? / error?` 结构；简单数据用 JSON `data`，复杂页面内容用 markdown/yaml `content`，错误放入 `error`。

- `bua_api2.ts` 定义：
  - `BuaClient`：`capabilities()`、`createSession()`。
  - `BuaSession`：聚合 `task / tabs / page / events / close()`。
  - `BuaTask`：`start / state / pause / resume / cancel / stop`，不包含 `interrupt`。
  - `BuaTabs`：`create / list / current / activate / close`。
    - `current()` 返回当前 active tab。
    - `activate(tabId)` 激活指定 tab 并返回它。
  - `BuaPage`：页面感知使用 `snapshot / screenshot`，跨页面导航使用 `navigate(url)`，页面内操作统一使用 `act(actions, options?)`。
  - `act()` 支持历史导航、点击、输入、滚动、鼠标移动、拖拽、选择和等待动作；不包含跨页面 `navigate` 动作。
  - `scroll` 动作需要传入目标元素或坐标，用于区分页面内多个可滚动区域。
  - 新的 BUA API 允许使用异常表达失败，但不机械地全部使用异常；按 JS API 惯例和具体接口语义决定返回值。
  - 查询类和操作类方法成功时直接返回领域对象；状态性操作可返回 `void`、布尔值或小结果对象。
  - Bridge 捕获新的 BUA API 异常并转换为 `BuaBridgeResult.error`，不把异常直接暴露给 LLM。

## Tool Contract Details
- 页面操作成功后返回操作摘要；若产生新页面状态，`content` 返回 markdown/yaml，`data.snapshot` 保留结构化对象。
- `bua_page_extract_content` 属于 Bridge 层能力：Bridge 读取 `page.snapshot()` 结果后构造成 markdown/yaml 内容，不要求新的 BUA API 提供同名方法。
- Bridge 中的页面操作类 tool 保持面向 LLM 的细粒度工具名：`bua_page_navigate` 转换为新的 BUA API `page.navigate()`，其他操作类 tool 转换为 `page.act()` 动作。
- `bua_take_over` 用于用户接管：Bridge 调新的 BUA API `task.pause()`，等待外部完成后调 `task.resume()`。
- `bua_page_wait` 支持 v1 条件：固定时间、页面稳定、URL 匹配、文本出现、元素出现。

## Test Plan
- 静态检查：
  - 确认两个新文件只导出类型/接口/常量，不引入 runtime side effect。
  - `rg` 检查不包含低优工具名：`page_search/eval/mouse/shortcut/scripttool`。
  - `git diff --check -- components/bua/bua_api_bridge.ts components/bua/bua_api2.ts components/bua/plan.md`。
- 类型契约检查：
  - 如果仓库当前没有 TS 编译入口，至少确保两个文件的 imports/exports 自洽。
- 设计验收：
  - Bridge tool registry 能覆盖文档列出的核心能力。
  - Bridge 不向 LLM 暴露 session/task id 作为必填概念。
  - Bridge 初始化时接收外部 host 提供的 `BuaSession`，不在 `invokeTool()` 中创建 session。
  - 新的 BUA API 能完整支撑 Bridge 的每个核心 tool。

## Assumptions
- `bua_api2.ts` 是未来替代旧 `bua_api.ts` 的新契约，但本次不删除、不迁移旧文件。
- Bridge tool schema 使用通用 JSON Schema，不绑定 OpenAI tools 或 MCP。
- 简单返回用 JSON；复杂页面内容用 markdown/yaml 文本，并可附带结构化 `data`。
- 低优工具本次完全不写，避免 LLM 看到未准备好的能力。
