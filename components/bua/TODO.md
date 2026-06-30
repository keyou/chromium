# BUA API 实现 TODO

最近审计时间：2026-06-29

这个文件用于维护 `components/bua/bua_api.ts` 中已经声明、但当前
Chromium BUA backend 尚未实现、只实现了一部分、或仍需要产品级验证的
公开契约。这里关注的是本地 BUA 产品 API，不关注是否适合提交到上游
Chromium。

## 标记说明

- `[ ]` 未实现。
- `[~]` 部分实现，或已经接线但仍有重要缺口。
- `[?]` 需要通过真实浏览器 / 侧边栏流程验证。
- `[x]` 对当前本地 demo 已经足够可用。

## Snapshot API

- `[x]` `BuaSnapshotOptions.mode`：`default` 和 `interact` 已经分别接到
  对应的 annotated page content 提取模式。
- `[~]` `BuaSnapshotOptions.channels`：content、screenshot、pdf 请求开关
  已经传给 fetcher，但 quality 字段还没有完整反映“哪些 channel 被请求”。
- `[~]` `BuaSnapshotOptions.budget`：`maxTextBytes`、`maxNodes`、
  `maxScreenshotWidth`、`maxScreenshotHeight` 和 PDF byte limit 已接线；
  `timeoutMs` 尚未接线。
- `[ ]` `BuaSnapshotOptions.purpose`：TS 契约已接受该字段，但 backend
  目前忽略它。
- `[~]` `BuaPageSnapshot.text`：已填充 `innerText`。当前 mojo 路径暂拿不到
  `innerTextOffset` 和 `passages`。
- `[~]` `BuaPageSnapshot.viewport`：已填充 width 和 height。尚未填充
  `scrollX`、`scrollY`、`deviceScaleFactor`。
- `[~]` `BuaPageSnapshot.quality`：已填充基础 channel quality，但
  requested / available / truncated 语义还需要更精确地反映禁用 channel、
  PDF 截断、超时和部分失败。
- `[~]` `BuaScreenshot`：viewport screenshot 以 data URI 返回。native bridge
  结果目前不暴露二进制 `data`。
- `[ ]` `BuaPageText.passages`：需要接入等价于 Glic PageContext
  `page_passages` 的来源，或实现 BUA 自己的 passage 提取路径。

## Page Node Model

- `[x]` snapshot node 到 actor DOM node target 的稳定 node 引用已实现；
  前提是节点有 document identifier 和 DOM node id。
- `[~]` `BuaPageNode.kind`：常见 APC node type 已映射，但 popup widget、
  更丰富的 dialog/list 语义以及部分控件仍需要覆盖测试。
- `[~]` `BuaPageNode.role`：当前使用数字 AX role 字符串，或者 fallback 到
  kind；后续应该暴露稳定的 BUA role 字符串。
- `[~]` `BuaPageNode.bounds`：已填充 visible bounds。outer bounds、
  clipped bounds、fragments、z-order 和坐标系元信息尚未填充。
- `[~]` `BuaNodeState`：已部分填充 `visible`、`offscreen`、`disabled`、
  `checked`、`editable`。尚未填充 `selected`、`focused`、`expanded`、
  `pressed`、`obscured`。
- `[x]` `BuaPageNode.actions`：已根据现有 APC 信号填充 click、hover、
  type、select、scroll 提示。
- `[x]` `BuaPageNode.scrollInfo`：APC 存在 scroller info 时，已填充基础
  scrollability、scrolling bounds 和 visible area。
- `[~]` `BuaFieldInfo`：已部分填充 field name、value、placeholder、
  required 和 select options。`label`、`type`、`autofillAvailable` 尚未完整
  填充。
- `[~]` `BuaFormInfo`：已部分填充 form name 和 action URL。`purpose` 和
  form label 尚未填充。
- `[ ]` `BuaTableInfo`：table、row、cell 的结构化元信息已声明，但尚未映射。
- `[ ]` `BuaMediaInfo` 和 `BuaMediaTranscript`：已声明，但尚未映射到 BUA
  page node。
- `[ ]` `BuaPageTool`：已声明，但尚未实现 frame / document tool 发现和
  action bridge。
- `[ ]` `BuaFrameInfo.metadata`：可以向 Glic 请求 max meta tags，但尚未映射到
  BUA frame node。
- `[ ]` `BuaPdfContent`：已检测 PDF 可用性，但尚未把 PDF bytes / content
  映射到 BUA frame data。
- `[?]` iframe node identity 和 action targeting 需要 browser test 覆盖
  same-origin 与 cross-origin frame。

## Target API

- `[x]` `targets.current`：返回 target snapshot 或结构化 no-target 状态。
- `[x]` `targets.list`：列出当前 profile 下 backend 可见的 target。
- `[~]` `BuaListTargetsOptions.includeBackground`：已接线。`includeClosed`
  被忽略。
- `[x]` `targets.createTab`：可在已有 browser window 中创建 HTTP(S) tab 和
  `about:blank`。
- `[x]` `targets.activate` 和 `targets.close`：tab target 的激活和关闭已接线。
- `[~]` `BuaTargetRef.windowId`：会解析到指定 window 的 active tab。
  `activeTab` 还没有作为显式选项建模。
- `[ ]` `targets.createWindow`、`activateWindow`、`closeWindow`：已声明，但
  当前 bridge 中有意保持 unsupported。
- `[~]` `BuaTargetSnapshot`：已部分填充 id、tabId、windowId、url、title、
  mimeType、active、readable、updatedAtMs。`focused`、`audible`、
  `mediaActive`、`captured` 和生命周期时间戳仍需要真实数据。
- `[ ]` `BuaWindowSnapshot`：已声明，但 native bridge 尚不生产该结构。

## Action API

- `[x]` `click`：node target 和 point target 已接线。
- `[x]` `type`：node target / point target、replace / append / prepend 以及
  submit 已接线。
- `[x]` `select`：node target / point target 与 selected value 已接线。
- `[x]` `scroll`：页面或 node / point target、direction、distance 已接线。
- `[x]` `scroll_to`：滚动到目标已接线。
- `[x]` `hover`：已映射到 actor mouse move。
- `[x]` `navigate`：HTTP(S) 导航已接线；没有 tab 时，navigate action 可以
  创建新 tab。
- `[~]` `history`：back 和 forward 已接线。reload 尚未接线。
- `[~]` `wait`：固定时间等待已接线。`navigation`、`page_stable`、`node`、
  `url_matches`、`text_present` 条件尚未接线。
- `[~]` `tab`：activate 和 close 已通过 actor action 接线；create 通过
  `targets.createTab` 暴露，不通过 `BuaTabAction`。
- `[ ]` `press`：已声明，但尚未映射到 actor action。
- `[ ]` `drag`：已声明，但尚未映射到 actor action。
- `[ ]` `window`：已声明，但尚未映射。
- `[ ]` `attempt_login`：已声明，但尚未实现。
- `[ ]` `attempt_form_fill`：已声明，但尚未实现。
- `[ ]` `attempt_otp_fill`：已声明，但尚未实现。
- `[ ]` `call_page_tool`：已声明，但尚未实现。
- `[ ]` `media`：已声明，但尚未实现。
- `[ ]` `capture`：已声明，但尚未作为 action 实现。截图能力目前通过
  `snapshot({channels: {screenshot: true}})` 提供。
- `[ ]` `yield_to_user`：已声明，但尚未实现。
- `[ ]` `BuaActionTarget.query`：当前明确 unsupported；调用方必须使用
  snapshot node 或 point target。
- `[~]` `BuaActOptions`：`target`、`mode`、`timeoutMs`、`snapshotAfter`、
  `stopOnFirstError` 已声明，但 native backend 尚未完整遵守。
- `[~]` `BuaActionResult`：已部分填充 ok / status / actionId / code /
  category / message / timing / diagnostics。native act 尚未填充 `effects`、
  `snapshot`、`pendingRequest`、`recovery`。

## Task API

- `[x]` `task.start`：创建或复用 backend actor task。
- `[x]` `task.state`：报告基础 running / idle 状态。
- `[x]` `task.stop` 和 `session.close`：停止当前 actor task。
- `[ ]` `task.pause`：facade 已声明，但 backend unsupported。
- `[ ]` `task.resume`：facade 已声明，但 backend unsupported。
- `[ ]` `task.interrupt`：TS API 已声明，但当前 facade / backend 路径未暴露。
- `[ ]` `task.cancelActions`：facade 已声明，但 backend unsupported。
- `[~]` `BuaTaskOptions`：除了 session id 和 task 创建外，id / title /
  userGoal / mode / target / timeout / metadata 尚未一致消费。
- `[~]` `BuaTaskState`：已填充基础 id / status / update time。action-level
  state 和更丰富的 reason 尚未填充。

## User Request API

- `[ ]` `requests.next`：当前返回无 pending request。
- `[ ]` `requests.respond`：当前是 no-op success。
- `[ ]` `requests.onRequest`：facade 返回 inert subscription。
- `[ ]` Credential selection request / response 已声明，但尚未实现。
- `[ ]` Autofill selection request / response 已声明，但尚未实现。
- `[ ]` User confirmation request / response 已声明，但尚未实现。
- `[ ]` Navigation confirmation request / response 已声明，但尚未实现。
- `[ ]` File picker request / response 已声明，但尚未实现。
- `[ ]` User takeover request / response 已声明，但尚未实现。

## Events and Diagnostics

- `[ ]` `BuaEventApi.on`：facade 返回 inert subscription；backend event
  delivery 尚未实现。
- `[ ]` `availability.onChange`：facade 返回 inert subscription。
- `[ ]` Snapshot invalidation event 已声明，但尚未发出。
- `[ ]` Target、task、action、permission、user request、diagnostic event
  已声明，但尚未发出。
- `[~]` Diagnostics 契约不一致：`bua_api.ts` 声明的是 `trace()`、`report()`、
  `onDiagnostic()`，当前 facade 暴露的是 `diagnostics.current()`。继续扩展
  diagnostics 前应先对齐这个 contract。
- `[ ]` `BuaTrace` 和 `BuaTraceEntry`：已声明，但尚未实现。

## SDK / Adapter Layer

- `[ ]` `components/bua` 中尚未实现一个消费 `BuaBackendAdapter`、并强制遵守
  public `BuaClient` contract 的 TypeScript SDK runtime。
- `[~]` renderer facade 实现了 browser global `window.bua`，但目前没有和
  `bua_api.ts` 做类型检查。
- `[ ]` `BuaBackendAdapter`、`BuaBackendTargetApi`、backend event type 和
  backend user request plumbing 目前仍停留在 interface-only 阶段。

## Verification Gaps

- `[ ]` 为 `snapshot({mode: "default"})` 和
  `snapshot({mode: "interact"})` 增加 native / browser test。
- `[ ]` 增加测试证明可滚动元素会暴露 `actions: ["scroll"]` 和 `scrollInfo`。
- `[ ]` 增加测试覆盖 no-tab navigate 自动创建新 tab。
- `[ ]` 增加测试覆盖 background tab 的 snapshot / act 行为。
- `[ ]` 增加测试覆盖 target list / create / activate / close。
- `[ ]` 增加测试覆盖 unsupported action 和 unsupported method 的错误结构。
- `[ ]` 增加测试保证 `bua_api.ts` 与 renderer facade 保持对齐。
