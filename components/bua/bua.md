**BUA 的本质**

BUA 不是“给 LLM 几个 click/type/read 工具”。更准确地说，它是一个闭环系统：

```text
任务输入
  -> 建立浏览器控制上下文
  -> 判断能力/权限/策略
  -> 选择目标 tab/window/page
  -> 观察页面
  -> 构造 LLM 可理解的状态
  -> LLM 决策
  -> 动作前校验
  -> 执行动作
  -> 页面稳定等待
  -> 动作后观察
  -> 判断动作结果
  -> 恢复 / 用户介入 / 继续 / 结束
```

所以 `ActionResult.TabObservation` 很重要，但它只覆盖了 **动作后观察**。完整 BUA 还需要动作前观察、状态订阅、权限判断、任务生命周期、用户确认、异常分类、恢复建议、日志与指标。

**BUA 主流程**

1. **Session / Task 启动**

   业务侧 agent 开始一个 Browser Use 任务。BUA runtime 需要建立一个可控上下文，包括当前浏览器实例、profile、tab/window、权限状态、可用能力、任务取消/暂停/恢复能力。

   这里不能假设底层一定有 Glic task。Glic 有 actor task，CDP 可能没有 task，但通用 BUA 必须有自己的 task 概念，用来支撑取消、超时、日志、恢复和资源释放。

2. **能力与权限检查**

   在真正读页面或操作页面前，需要知道当前环境是否允许：

   - 是否有可观察 tab
   - 是否允许读取页面上下文
   - 是否允许 act on web
   - 是否受企业策略、Safe Browsing、站点风险、scheme 限制影响
   - 是否需要用户授权或设置变更
   - 是否支持截图、APC/AX/DOM、form filling、credential、script tool、media、tab/window 管理等能力

   这一层决定“能不能做”和“能做到什么程度”，不应该等 action 失败后才发现。

3. **目标选择**

   BUA 要先明确操作对象：

   - 当前 focused tab
   - 指定 tab
   - 新 tab
   - pinned/shared tab
   - actor/automation 专用 tab
   - 多窗口/多 tab 场景

   目标选择不是小问题。很多失败来自 tab 被切走、页面不可观察、用户导航、窗口失活、tab 销毁、focus candidate 不可用。

4. **页面观察**

   Observation 至少有几类来源：

   - 主动读取：DOM / AX / APC / innerText / screenshot / metadata / PDF
   - 动作后返回：ActionResult 中的 TabObservation、WindowObservation、latency、error
   - 状态订阅：focused tab、tab data、task state、permission/capability、page metadata
   - 用户附加上下文：region selection、text selection、drag/drop image、shared context

   这些来源不应该直接暴露给业务侧，而应该汇聚成“当前页面状态”。

5. **状态构造**

   BUA runtime 要把底层观察结果变成 LLM 可用状态：

   - 当前页面 URL/title/mime/load-ish 状态
   - 可操作元素列表
   - 元素 role/name/text/value/bounds/disabled/checked/visible
   - 页面文本摘要
   - 截图或截图引用
   - frame/iframe/shadow DOM 的必要信息
   - 表单、登录、OTP、autofill、media、script tool 等特殊能力线索
   - observation 是否完整、是否过期、是否截断、是否缺截图/APC

   这里的目标是提高 LLM 成功率，不是完整还原底层数据结构。

6. **LLM 决策**

   业务侧 LLM agent 根据状态决定下一步。BUA 不负责替业务写 agent 策略，但 BUA 必须提供足够清楚、稳定、低噪声的状态，让业务侧容易包装成 tools。

7. **动作前校验**

   执行动作前，BUA 需要做防错：

   - element 是否来自当前 observation
   - observation 是否过期
   - tab/frame/document 是否仍然匹配
   - 元素是否可见、未禁用、未被遮挡、可交互
   - 坐标是否在 viewport 内
   - 目标页面是否仍允许 actuation
   - 是否可能触发跨域导航、下载、外部协议、文件选择器等敏感行为

   这一步决定稳定性。没有它，LLM 会频繁点旧元素、点错 frame、点到被遮挡位置。

8. **动作执行**

   动作能力不应只覆盖 click/type/scroll。完整 BUA 至少要考虑：

   - page action：click、type、select、scroll、hover/move、drag、press、wait
   - navigation：navigate、back、forward、reload、等待导航
   - tab/window：create、close、activate、pin/unpin
   - form/login：attempt login、attempt autofill、OTP filling
   - script tool：页面声明式工具或受控脚本能力
   - media：play、pause、seek
   - capture：screenshot、区域选择、文本/图片上下文
   - user handoff：yield、pause、resume、interrupt

9. **页面稳定与动作后观察**

   动作结束不代表页面可继续推理。BUA 需要判断是否等待页面稳定：

   - 动作是否修改页面
   - 是否触发导航
   - 是否有异步 DOM 更新
   - 是否需要截图/APC
   - 是否 observation 超时或页面变化导致放弃
   - 是否需要 retry observation

   Glic 的 `ActionsResult.TabObservation` 正好属于这一阶段，是非常有价值的优化，因为它避免了每次 action 后再额外 read context。

10. **结果判定**

   BUA 要把底层错误归类成 agent 可恢复的语义：

   - target 不存在
   - target stale
   - element disabled/offscreen/obscured
   - tab/window/frame gone
   - navigation blocked/failed/cross-origin
   - task paused/stopped/cancelled
   - timeout
   - permission/policy blocked
   - credential/autofill/user confirmation needed
   - renderer/page crashed
   - backend unsupported/error

   业务侧不应该直接消费 Glic `ActionResultCode` 或 CDP exception，而是消费稳定的错误分类和恢复建议。

11. **恢复与用户介入**

   失败后不只是返回 error。BUA 应该告诉 agent 下一步怎么恢复：

   - 重新 observe
   - scroll 后重试
   - 换目标元素
   - 等待页面稳定
   - 请求用户确认
   - 请求用户选择 credential/autofill suggestion
   - 暂停/交还用户
   - 停止 task
   - 降级为截图/坐标操作

12. **结束与清理**

   任务结束时需要：

   - cancel ongoing actions
   - stop/pause/resume task
   - unsubscribe 状态观察
   - 失效 element refs
   - 释放截图/blob/stream
   - 写 journal/metrics/trace
   - 输出最终状态和失败诊断

**能力边界**

我建议先把 BUA 能力边界分成这些层：

```text
Agent 层
  业务侧 LLM、prompt、任务规划、业务判断

BUA Runtime 层
  session/task、状态管理、观察归一化、动作执行、错误归类、恢复建议

Backend Adapter 层
  Glic / CDP / Playwright / WebDriver / native browser control

Browser Kernel 层
  DOM、AX、screenshot、input dispatch、navigation、tab/window、policy/security
```

BUA Runtime 应该负责：

- 统一页面感知模型
- 统一动作模型
- 统一 action result
- 统一错误和恢复语义
- 统一 task/session 生命周期
- 维护 element ref 生命周期
- 做动作前 TOCTOU 校验
- 做动作后 observation 归并
- 屏蔽 Glic/CDP 差异

BUA Runtime 不应该负责：

- 业务任务规划
- LLM prompt 策略
- 业务侧 tool 注册格式
- 保存或决定敏感凭据
- 绕过浏览器安全策略
- 暴露底层 proto/node id/session id 作为 public contract

**关键结论**

通用 BUA API 设计前，必须先承认：

1. BUA 是闭环，不是 action 方法集合。
2. observation 不是单个接口，而是感知、动作反馈、订阅状态的汇聚结果。
3. action result 必须天然携带动作后 observation。
4. 错误模型必须是一级能力，否则 LLM 无法稳定恢复。
5. task/session 生命周期必须存在，即使某些 backend 没有原生 task。
6. credential/autofill/navigation confirmation/user handoff 不是边缘功能，而是 Browser Use 成功率的重要组成。
7. Glic 是能力很强的 backend，但 public BUA 模型不能长成 Glic 的样子。

我先停在这里，不继续设计 API。下一步应该先让你 review 这套 BUA 流程和能力边界，看它是否已经覆盖你认为的 Browser Use 核心闭环。