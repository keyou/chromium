// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

/**
 * Generic Browser Use Automation API surface.
 *
 * This file intentionally does not depend on any browser-specific protocol.
 * Business agents should integrate with these interfaces and let backend
 * adapters translate to the underlying technology.
 *
 * 简单使用示例：
 *
 * ```ts
 * const client = createBuaClient(adapter);
 * const session = await client.createSession({
 *   defaultSnapshot: {purpose: 'plan'},
 * });
 *
 * const availability = await session.availability.current();
 * if (!availability.canReadPage.ok || !availability.canAct.ok) {
 *   throw new Error(availability.canReadPage.reason ?? availability.canAct.reason);
 * }
 *
 * const snapshot = await session.snapshot({
 *   purpose: 'plan',
 *   channels: {content: true, screenshot: true},
 * });
 *
 * const findNode = (
 *     node: BuaPageNode,
 *     match: (candidate: BuaPageNode) => boolean): BuaPageNode|undefined =>
 *   match(node) ? node :
 *       node.children?.map(child => findNode(child, match)).find(Boolean);
 *
 * const searchBox = snapshot.content &&
 *     findNode(snapshot.content, node =>
 *       node.role === 'textbox' && node.name?.includes('搜索'));
 *
 * if (searchBox) {
 *   const result = await session.act([
 *     {
 *       kind: 'type',
 *       target: {nodeId: searchBox.id},
 *       text: 'Chromium BUA',
 *       submit: true,
 *     },
 *   ], {snapshotAfter: 'auto'});
 *
 *   if (!result.ok && result.pendingRequest) {
 *     await session.requests.respond(result.pendingRequest.id, {
 *       kind: 'user_confirmation',
 *       granted: true,
 *     });
 *   }
 * }
 *
 * await session.close('task_finished');
 * ```
 */

export type BuaId = string;
export type BuaTimestampMs = number;

/** BUA SDK 的顶层入口，用于查询整体能力并创建独立的浏览器使用会话。 */
export interface BuaClient {
  /** 查询当前 SDK/backend 理论支持的能力，不代表当前页面一定可用。 */
  capabilities(): Promise<BuaCapabilities>;
  /** 创建一次 Browser Use 会话，业务侧通常为一个 agent 任务创建一个 session。 */
  createSession(options?: BuaSessionOptions): Promise<BuaSession>;
}

/** 一次 Browser Use 会话，聚合感知、动作、任务状态、用户请求和诊断能力。 */
export interface BuaSession {
  readonly id: BuaId;
  readonly capabilities: BuaCapabilities;

  readonly availability: BuaAvailabilityApi;
  readonly task: BuaTaskApi;
  readonly targets: BuaTargetApi;
  readonly requests: BuaUserRequestApi;
  readonly events: BuaEventApi;
  readonly diagnostics: BuaDiagnosticsApi;

  /** 主动读取一次当前页面状态，返回适合 LLM 消费的页面快照。 */
  snapshot(options?: BuaSnapshotOptions): Promise<BuaPageSnapshot>;
  /** 返回最近一次有效或已标记失效的页面快照，调用方需要检查 quality/stale。 */
  latestSnapshot(): BuaPageSnapshot | undefined;

  /** 执行一个或多个浏览器动作，并按配置返回动作后的页面观察。 */
  act(
      actions: BuaAction|readonly BuaAction[],
      options?: BuaActOptions): Promise<BuaActionResult>;

  /** 关闭会话并释放订阅、观察缓存和 backend 资源。 */
  close(reason?: string): Promise<void>;
}

/** 创建会话时的配置，主要用于设置默认目标和默认观察策略。 */
export interface BuaSessionOptions {
  id?: BuaId;
  metadata?: BuaMetadata;
  defaultTarget?: BuaTargetRef;
  defaultSnapshot?: BuaSnapshotOptions;
}

export type BuaMetadata = Record<string, unknown>;

/** backend 声明的静态能力集合，用来决定 SDK 能暴露哪些功能路径。 */
export interface BuaCapabilities {
  readonly backend: BuaBackendInfo;
  readonly snapshot: BuaSnapshotCapability;
  readonly act: BuaActCapability;
  readonly targets: BuaTargetCapability;
  readonly userRequests: BuaUserRequestCapability;
  readonly events: readonly BuaEventType[];
}

/** backend 的基本信息，仅用于诊断和能力展示，不参与业务决策。 */
export interface BuaBackendInfo {
  readonly name: string;
  readonly version?: string;
  readonly protocol?: string;
}

/** 页面感知通道的支持情况，content 表示递归页面语义树。 */
export interface BuaSnapshotCapability {
  readonly content: boolean;
  readonly screenshot: boolean;
  readonly metadata: boolean;
  readonly pdf: boolean;
}

/** 浏览器动作能力的支持情况，包括动作集合、序列执行和任务控制能力。 */
export interface BuaActCapability {
  readonly actions: readonly BuaActionKind[];
  readonly sequences: boolean;
  readonly snapshotAfterAction: boolean;
  readonly cancel: boolean;
  readonly pause: boolean;
  readonly resume: boolean;
}

/** 目标页签和窗口管理能力，v1 只保留核心 tab/window 操作。 */
export interface BuaTargetCapability {
  readonly current: boolean;
  readonly list: boolean;
  readonly createTab: boolean;
  readonly activateTab: boolean;
  readonly closeTab: boolean;
  readonly createWindow: boolean;
  readonly activateWindow: boolean;
  readonly closeWindow: boolean;
}

/** 需要用户或业务 UI 介入的请求类型支持情况。 */
export interface BuaUserRequestCapability {
  readonly credentialSelection: boolean;
  readonly autofillSelection: boolean;
  readonly userConfirmation: boolean;
  readonly navigationConfirmation: boolean;
  readonly filePicker: boolean;
  readonly userTakeover: boolean;
}

/** 当前可用性 API，用于判断“现在能不能看页面、能不能操作页面”。 */
export interface BuaAvailabilityApi {
  /** 查询当前权限、策略、焦点和 backend 状态。 */
  current(): Promise<BuaAvailability>;
  /** 监听可用性变化，例如权限被关闭、目标页签切换或策略变化。 */
  onChange(handler: (availability: BuaAvailability) => void): BuaSubscription;
}

/** 当前 Browser Use 环境的运行时可用性，不等同于静态 capabilities。 */
export interface BuaAvailability {
  readonly status: 'available'|'degraded'|'unavailable';
  readonly canReadPage: BuaAvailabilityCheck;
  readonly canAct: BuaAvailabilityCheck;
  readonly focusedTarget: BuaAvailabilityCheck;
  readonly permissions: readonly BuaPermissionState[];
  readonly policies: readonly BuaPolicyState[];
  readonly diagnostics?: readonly BuaDiagnostic[];
}

/** 某个可用性检查项的结果，并可附带恢复建议。 */
export interface BuaAvailabilityCheck {
  readonly ok: boolean;
  readonly reason?: string;
  readonly recovery?: readonly BuaRecoveryHint[];
}

/** 单项权限状态，例如页面上下文读取、浏览器操作、截图或凭据能力。 */
export interface BuaPermissionState {
  readonly name: BuaPermissionName;
  readonly state: 'granted'|'denied'|'prompt'|'unknown';
  readonly source?: 'browser'|'os'|'profile'|'enterprise'|'backend';
}

export type BuaPermissionName =
  | 'page_context'
  | 'browser_actuation'
  | 'screenshot'
  | 'credential'
  | 'autofill'
  | 'file_system'
  | 'microphone'
  | 'location';

/** 单项策略状态，例如企业策略、安全浏览、站点风险或 scheme 限制。 */
export interface BuaPolicyState {
  readonly name: BuaPolicyName;
  readonly state: 'allowed'|'blocked'|'unknown';
  readonly reason?: string;
}

export type BuaPolicyName =
  | 'safe_browsing'
  | 'enterprise'
  | 'site_risk'
  | 'scheme'
  | 'cross_origin_navigation'
  | 'sensitive_content';

/** 任务生命周期 API，用来支持长任务、取消、暂停、恢复和用户介入。 */
export interface BuaTaskApi {
  /** 开始一个 Browser Use 任务；backend 没有原生 task 时可由 SDK 模拟。 */
  start(options?: BuaTaskOptions): Promise<BuaTaskState>;
  /** 获取当前任务状态。 */
  state(): Promise<BuaTaskState>;
  /** 暂停任务并取消或冻结正在执行的动作。 */
  pause(reason?: string): Promise<BuaTaskState>;
  /** 恢复暂停的任务，可选择恢复时重新观察页面。 */
  resume(options?: BuaResumeOptions): Promise<BuaTaskState>;
  /** 标记任务等待外部输入，但不一定完全暂停任务资源。 */
  interrupt(reason?: BuaInterruptReason): Promise<BuaTaskState>;
  /** 停止任务并清理任务相关状态。 */
  stop(reason?: BuaStopReason): Promise<BuaStopResult>;
  /** 取消正在执行的动作序列，不表示回滚已经完成的动作。 */
  cancelActions(reason?: string): Promise<BuaCancelResult>;
}

/** 启动任务时的业务上下文和运行约束。 */
export interface BuaTaskOptions {
  title?: string;
  userGoal?: string;
  mode?: 'default'|'fast';
  target?: BuaTargetRef;
  timeoutMs?: number;
  metadata?: BuaMetadata;
}

/** 恢复任务时的选项，常用于要求恢复后立即刷新页面观察。 */
export interface BuaResumeOptions {
  snapshot?: boolean|BuaSnapshotOptions;
}

export type BuaInterruptReason =
  | 'needs_user_input'
  | 'needs_confirmation'
  | 'needs_credential'
  | 'needs_external_action'
  | 'other';

export type BuaStopReason =
  | 'completed'
  | 'cancelled'
  | 'failed'
  | 'user_requested'
  | 'session_closed'
  | 'other';

/** 任务当前状态，业务侧可用它决定是否继续调用 snapshot/act。 */
export interface BuaTaskState {
  readonly id: BuaId;
  readonly status:
      'idle'|'running'|'acting'|'paused'|'interrupted'|'stopping'|'stopped';
  readonly startedAtMs?: BuaTimestampMs;
  readonly updatedAtMs: BuaTimestampMs;
  readonly reason?: string;
  readonly currentActionId?: BuaId;
}

/** 停止任务的结果，包含最终任务状态和可能的诊断信息。 */
export interface BuaStopResult {
  readonly ok: boolean;
  readonly state: BuaTaskState;
  readonly diagnostics?: readonly BuaDiagnostic[];
}

/** 取消当前动作的结果；没有动作可取消时不应视为异常。 */
export interface BuaCancelResult {
  readonly ok: boolean;
  readonly status: 'cancelled'|'nothing_to_cancel'|'failed';
  readonly reason?: string;
}

/** 目标管理 API，负责选择和管理当前可观察、可操作的 tab/window。 */
export interface BuaTargetApi {
  /** 获取当前默认目标；没有可用目标时返回 no_target 状态。 */
  current(): Promise<BuaTargetSnapshot|BuaNoTargetState>;
  /** 列出当前 backend 可见的目标。 */
  list(options?: BuaListTargetsOptions): Promise<readonly BuaTargetSnapshot[]>;

  /** 创建新 tab，是否激活由 options.background 决定。 */
  createTab(options?: BuaCreateTabOptions): Promise<BuaTargetSnapshot>;
  /** 激活指定目标，使后续默认 snapshot/act 可以面向该目标。 */
  activate(target: BuaTargetRef): Promise<void>;
  /** 关闭指定目标。 */
  close(target: BuaTargetRef): Promise<void>;

  /** 创建新窗口，v1 仅表达核心能力，不包含窗口布局控制。 */
  createWindow(options?: BuaCreateWindowOptions): Promise<BuaWindowSnapshot>;
  /** 激活指定窗口。 */
  activateWindow(windowId: BuaId): Promise<void>;
  /** 关闭指定窗口。 */
  closeWindow(windowId: BuaId): Promise<void>;
}

export type BuaTargetRef =
  | {targetId: BuaId}
  | {tabId: BuaId}
  | {windowId: BuaId; activeTab?: boolean}
  | {current: true};

/** 查询目标列表时的过滤条件。 */
export interface BuaListTargetsOptions {
  includeBackground?: boolean;
  includeClosed?: boolean;
  windowId?: BuaId;
}

/** 创建 tab 的基础选项。 */
export interface BuaCreateTabOptions {
  url?: string;
  windowId?: BuaId;
  background?: boolean;
}

/** 创建窗口的基础选项。 */
export interface BuaCreateWindowOptions {
  url?: string;
  background?: boolean;
}

/** 没有可用目标时的结构化状态，避免用异常承载普通运行状态。 */
export interface BuaNoTargetState {
  readonly kind: 'no_target';
  readonly reason: 'no_browser'|'no_focusable_target'|'target_not_readable'|'unknown';
  readonly candidate?: BuaTargetSnapshot;
  readonly message?: string;
  readonly recovery?: readonly BuaRecoveryHint[];
}

/** tab/window/page 的稳定快照，用于观察结果、事件和动作结果。 */
export interface BuaTargetSnapshot {
  readonly id: BuaId;
  readonly kind: 'page'|'tab'|'window'|'browser';
  readonly tabId?: BuaId;
  readonly windowId?: BuaId;
  readonly url?: string;
  readonly title?: string;
  readonly mimeType?: string;
  readonly active?: boolean;
  readonly focused?: boolean;
  readonly readable?: boolean;
  readonly audible?: boolean;
  readonly mediaActive?: boolean;
  readonly captured?: boolean;
  readonly createdAtMs?: BuaTimestampMs;
  readonly updatedAtMs?: BuaTimestampMs;
}

/** 浏览器窗口的轻量快照。 */
export interface BuaWindowSnapshot {
  readonly id: BuaId;
  readonly active?: boolean;
  readonly activeTargetId?: BuaId;
  readonly targetIds: readonly BuaId[];
}

/** 页面内容提取模式，default 偏整体内容，interact 偏可操作元素。 */
export type BuaSnapshotMode = 'default'|'interact';

/** 主动读取页面快照时的配置，决定目标、模式、目的、通道和资源预算。 */
export interface BuaSnapshotOptions {
  target?: BuaTargetRef;
  mode?: BuaSnapshotMode;
  purpose?: 'plan'|'verify'|'recover'|'full'|'fast';
  channels?: BuaSnapshotChannels;
  budget?: BuaSnapshotBudget;
}

/** 页面快照通道开关；业务侧可按任务需要控制成本和速度。 */
export interface BuaSnapshotChannels {
  /** 递归页面语义树，包含文本、可操作节点、表单、媒体、frame tools 等信息。 */
  content?: boolean;
  screenshot?: boolean;
  metadata?: boolean;
  pdf?: boolean;
}

/** 单次页面快照的资源预算，用于控制超时、文本大小、元素数量和截图尺寸。 */
export interface BuaSnapshotBudget {
  timeoutMs?: number;
  maxBytes?: number;
  maxTextBytes?: number;
  maxNodes?: number;
  maxScreenshotWidth?: number;
  maxScreenshotHeight?: number;
}

/** 页面快照结果，是 BUA 给 LLM/业务侧的核心页面状态模型。 */
export interface BuaPageSnapshot {
  readonly id: BuaId;
  readonly mode: BuaSnapshotMode;
  readonly source: 'explicit'|'after_action'|'resume'|'event';
  readonly createdAtMs: BuaTimestampMs;
  readonly generation: number;

  readonly target: BuaTargetSnapshot;
  readonly page: BuaPageInfo;
  readonly viewport?: BuaViewport;
  readonly text?: BuaPageText;
  readonly content?: BuaPageNode;
  readonly screenshot?: BuaScreenshot;

  readonly quality: BuaSnapshotQuality;
  readonly diagnostics?: readonly BuaDiagnostic[];
}

/** 页面文本通道，对齐浏览器页面上下文中的可读文本事实。 */
export interface BuaPageText {
  readonly innerText?: string;
  readonly innerTextOffset?: number;
  readonly passages?: readonly string[];
}

/** 页面自身的轻量状态，避免业务侧从 target 中重复推断页面情况。 */
export interface BuaPageInfo {
  readonly url?: string;
  readonly title?: string;
  readonly origin?: string;
  readonly mimeType?: string;
  readonly loading?: boolean;
  readonly crashed?: boolean;
  readonly errorPage?: boolean;
  readonly pdf?: boolean;
}

/** 当前可视区域信息，坐标类动作和截图解释都依赖这个结构。 */
export interface BuaViewport {
  readonly width: number;
  readonly height: number;
  readonly scrollX?: number;
  readonly scrollY?: number;
  readonly deviceScaleFactor?: number;
}

/** 递归页面节点，表达页面语义树，也是动作引用的核心对象。 */
export interface BuaPageNode {
  readonly id: BuaId;
  readonly snapshotId: BuaId;
  readonly kind: BuaPageNodeKind;
  readonly role?: string;
  readonly name?: string;
  readonly text?: string;
  readonly value?: string;
  readonly bounds?: BuaRect;
  readonly state?: BuaNodeState;
  readonly actions?: readonly BuaActionKind[];
  readonly scrollInfo?: BuaScrollInfo;
  readonly confidence?: number;

  readonly frame?: BuaFrameInfo;
  readonly form?: BuaFormInfo;
  readonly field?: BuaFieldInfo;
  readonly table?: BuaTableInfo;
  readonly media?: BuaMediaInfo;

  readonly children?: readonly BuaPageNode[];
}

/** 页面节点类型，frame 同时表达主页面和 iframe 的上下文边界。 */
export type BuaPageNodeKind =
  | 'frame'
  | 'section'
  | 'dialog'
  | 'text'
  | 'image'
  | 'link'
  | 'button'
  | 'input'
  | 'textarea'
  | 'select'
  | 'option'
  | 'checkbox'
  | 'radio'
  | 'form'
  | 'table'
  | 'row'
  | 'cell'
  | 'list'
  | 'list_item'
  | 'media'
  | 'canvas'
  | 'unknown';

/** 节点当前交互状态，用于动作前判断和失败恢复。 */
export interface BuaNodeState {
  readonly visible?: boolean;
  readonly disabled?: boolean;
  readonly checked?: boolean;
  readonly selected?: boolean;
  readonly focused?: boolean;
  readonly editable?: boolean;
  readonly expanded?: boolean;
  readonly pressed?: boolean;
  readonly obscured?: boolean;
  readonly offscreen?: boolean;
}

/** 可滚动节点的滚动范围和当前可见区域。 */
export interface BuaScrollInfo {
  readonly scrollableX?: boolean;
  readonly scrollableY?: boolean;
  readonly scrollingBounds?: BuaSize;
  readonly visibleArea?: BuaRect;
}

/** frame 信息。根节点 main=true，iframe 子树 main=false。 */
export interface BuaFrameInfo {
  /** BUA 生成的稳定 document 引用，backend 可映射到底层 document identity。 */
  readonly documentId?: BuaId;
  readonly main?: boolean;
  readonly url?: string;
  readonly title?: string;
  readonly origin?: string;
  readonly metadata?: readonly BuaPageMetadata[];
  readonly pdf?: BuaPdfContent;
  readonly tools?: readonly BuaPageTool[];
}

/** 页面或 frame 中的 meta 信息。 */
export interface BuaPageMetadata {
  readonly frameUrl?: string;
  readonly name: string;
  readonly content: string;
}

/** PDF 页面内容，data 可能因大小限制被省略。 */
export interface BuaPdfContent {
  readonly origin?: string;
  readonly data?: ArrayBuffer;
  readonly sizeLimitExceeded?: boolean;
}

/** 表单节点的高阶信息。字段本身通过子节点的 field 信息表达。 */
export interface BuaFormInfo {
  readonly purpose?: string;
  readonly label?: string;
  readonly name?: string;
  readonly actionUrl?: string;
}

/** 表单字段节点的高阶信息，适用于 input/textarea/select/checkbox/radio 等。 */
export interface BuaFieldInfo {
  readonly name?: string;
  readonly label?: string;
  readonly type?: string;
  readonly placeholder?: string;
  readonly required?: boolean;
  readonly autofillAvailable?: boolean;
  readonly options?: readonly BuaSelectOption[];
}

/** select/combobox 等控件中的选项信息。 */
export interface BuaSelectOption {
  readonly value?: string;
  readonly label?: string;
  readonly selected?: boolean;
}

/** 表格、行或单元格节点的结构信息。 */
export interface BuaTableInfo {
  readonly rowCount?: number;
  readonly columnCount?: number;
  readonly rowIndex?: number;
  readonly columnIndex?: number;
  readonly rowSpan?: number;
  readonly columnSpan?: number;
  readonly header?: boolean;
}

/** 媒体节点状态，例如音视频是否播放以及进度。 */
export interface BuaMediaInfo {
  readonly kind?: 'audio'|'video'|'unknown';
  readonly active?: boolean;
  readonly paused?: boolean;
  readonly durationMs?: number;
  readonly currentTimeMs?: number;
  readonly title?: string;
  readonly artist?: string;
  readonly album?: string;
  readonly transcripts?: readonly BuaMediaTranscript[];
}

/** 媒体转写片段。 */
export interface BuaMediaTranscript {
  readonly text: string;
  readonly startTimeMs?: number;
}

/** 二维尺寸。 */
export interface BuaSize {
  readonly width: number;
  readonly height: number;
}

/** 页面坐标矩形，默认使用 viewport 坐标系。 */
export interface BuaRect {
  readonly x: number;
  readonly y: number;
  readonly width: number;
  readonly height: number;
}

/** 页面坐标点，默认使用 viewport 坐标系。 */
export interface BuaPoint {
  readonly x: number;
  readonly y: number;
}

/** 截图数据或截图引用；实现可选择返回二进制数据或 URI。 */
export interface BuaScreenshot {
  readonly id?: BuaId;
  readonly width: number;
  readonly height: number;
  readonly mimeType: string;
  readonly data?: ArrayBuffer;
  readonly uri?: string;
}

/** frame/document 暴露给 agent 的结构化页面工具，不作为普通页面节点。 */
export interface BuaPageTool {
  readonly name: string;
  readonly description?: string;
  readonly inputSchema?: BuaJsonSchema;
  readonly annotations?: {
    readonly readOnly?: boolean;
    readonly untrustedContent?: boolean;
  };
}

export type BuaJsonSchema = Record<string, unknown>;

/** 页面快照质量，用于说明快照是否完整、是否过期、是否部分失败。 */
export interface BuaSnapshotQuality {
  readonly ok: boolean;
  readonly stale?: boolean;
  readonly partial?: boolean;
  readonly content?: BuaChannelQuality;
  readonly screenshot?: BuaChannelQuality;
  readonly metadata?: BuaChannelQuality;
  readonly pdf?: BuaChannelQuality;
  readonly reason?: string;
}

/** 单个观察通道的质量状态。 */
export interface BuaChannelQuality {
  readonly requested: boolean;
  readonly available: boolean;
  readonly truncated?: boolean;
  readonly timedOut?: boolean;
  readonly error?: string;
}

/** BUA 支持的动作联合类型，覆盖核心页面操作和少量高阶 Browser Use 动作。 */
export type BuaAction =
  | BuaClickAction
  | BuaTypeAction
  | BuaPressAction
  | BuaSelectAction
  | BuaScrollAction
  | BuaScrollToAction
  | BuaHoverAction
  | BuaDragAction
  | BuaNavigateAction
  | BuaHistoryAction
  | BuaWaitAction
  | BuaTabAction
  | BuaWindowAction
  | BuaAttemptLoginAction
  | BuaAttemptFormFillAction
  | BuaAttemptOtpFillAction
  | BuaCallPageToolAction
  | BuaMediaAction
  | BuaCaptureAction
  | BuaYieldToUserAction;

/** 动作类型名称，常用于 capabilities、节点支持动作和诊断信息。 */
export type BuaActionKind =
  | 'click'
  | 'type'
  | 'press'
  | 'select'
  | 'scroll'
  | 'scroll_to'
  | 'hover'
  | 'drag'
  | 'navigate'
  | 'history'
  | 'wait'
  | 'tab'
  | 'window'
  | 'attempt_login'
  | 'attempt_form_fill'
  | 'attempt_otp_fill'
  | 'call_page_tool'
  | 'media'
  | 'capture'
  | 'yield_to_user';

/** 动作目标。优先使用 nodeId，query/point 作为补充或降级路径。 */
export type BuaActionTarget =
  | {readonly nodeId: BuaId}
  | {readonly point: BuaPoint}
  | {readonly query: BuaNodeQuery};

/** 通过类型、角色、名称或文本查找页面节点时使用的轻量查询条件。 */
export interface BuaNodeQuery {
  kind?: BuaPageNodeKind;
  role?: string;
  name?: string;
  text?: string;
}

/** 所有动作共享的基础字段。 */
export interface BuaBaseAction {
  readonly id?: BuaId;
  readonly target?: BuaActionTarget;
  readonly targetRef?: BuaTargetRef;
  readonly timeoutMs?: number;
  readonly metadata?: BuaMetadata;
}

/** 鼠标点击动作。 */
export interface BuaClickAction extends BuaBaseAction {
  readonly kind: 'click';
  readonly target: BuaActionTarget;
  readonly button?: 'left'|'right';
  readonly count?: 1|2;
}

/** 文本输入动作，支持替换、追加、前置和输入后提交。 */
export interface BuaTypeAction extends BuaBaseAction {
  readonly kind: 'type';
  readonly target: BuaActionTarget;
  readonly text: string;
  readonly mode?: 'replace'|'append'|'prepend';
  readonly submit?: boolean;
}

/** 按键动作，可用于 Enter、Escape、Tab 或带修饰键的快捷键。 */
export interface BuaPressAction extends BuaBaseAction {
  readonly kind: 'press';
  readonly key: string;
  readonly modifiers?: readonly BuaKeyModifier[];
}

export type BuaKeyModifier = 'alt'|'ctrl'|'meta'|'shift';

/** 下拉框选择动作。 */
export interface BuaSelectAction extends BuaBaseAction {
  readonly kind: 'select';
  readonly target: BuaActionTarget;
  readonly value: string;
}

/** 滚动动作，可对页面或具体元素滚动。 */
export interface BuaScrollAction extends BuaBaseAction {
  readonly kind: 'scroll';
  readonly target?: BuaActionTarget;
  readonly direction: 'up'|'down'|'left'|'right';
  readonly distance?: number;
}

/** 将目标元素滚动到可视区域的动作。 */
export interface BuaScrollToAction extends BuaBaseAction {
  readonly kind: 'scroll_to';
  readonly target: BuaActionTarget;
}

/** 鼠标悬停动作，常用于展开菜单或触发 tooltip。 */
export interface BuaHoverAction extends BuaBaseAction {
  readonly kind: 'hover';
  readonly target: BuaActionTarget;
}

/** 拖拽动作，从一个目标拖到另一个目标。 */
export interface BuaDragAction extends BuaBaseAction {
  readonly kind: 'drag';
  readonly from: BuaActionTarget;
  readonly to: BuaActionTarget;
}

/** 导航到指定 URL 的动作。 */
export interface BuaNavigateAction extends BuaBaseAction {
  readonly kind: 'navigate';
  readonly url: string;
}

/** 历史导航动作，包括后退、前进和刷新。 */
export interface BuaHistoryAction extends BuaBaseAction {
  readonly kind: 'history';
  readonly direction: 'back'|'forward'|'reload';
}

/** 等待动作，用于等待时间、导航、页面稳定或特定内容出现。 */
export interface BuaWaitAction extends BuaBaseAction {
  readonly kind: 'wait';
  readonly waitMs?: number;
  readonly until?: BuaWaitCondition;
}

/** 等待条件，避免业务侧只能用固定 sleep。 */
export type BuaWaitCondition =
  | {type: 'time'; waitMs: number}
  | {type: 'navigation'}
  | {type: 'page_stable'}
  | {type: 'node'; query: BuaNodeQuery}
  | {type: 'url_matches'; pattern: string}
  | {type: 'text_present'; text: string};

/** tab 基础操作动作。v1 不包含 pin/unpin/lock 等目标稳定扩展。 */
export interface BuaTabAction extends BuaBaseAction {
  readonly kind: 'tab';
  readonly operation: 'create'|'activate'|'close';
  readonly url?: string;
}

/** window 基础操作动作。 */
export interface BuaWindowAction extends BuaBaseAction {
  readonly kind: 'window';
  readonly operation: 'create'|'activate'|'close';
  readonly windowId?: BuaId;
  readonly url?: string;
}

/** 高阶登录尝试动作，具体凭据选择通过 user request 完成。 */
export interface BuaAttemptLoginAction extends BuaBaseAction {
  readonly kind: 'attempt_login';
  readonly loginTargets?: readonly BuaLoginTarget[];
}

/** 登录入口目标，例如密码表单提交按钮或 federated sign-in 按钮。 */
export interface BuaLoginTarget {
  readonly type: 'password_form_submit'|'federated_google_signin'|'other';
  readonly target: BuaActionTarget;
}

/** 高阶表单填充动作，适合地址、信用卡、联系信息等浏览器可托管数据。 */
export interface BuaAttemptFormFillAction extends BuaBaseAction {
  readonly kind: 'attempt_form_fill';
  readonly requests: readonly BuaFormFillRequest[];
}

/** 单个表单填充请求，描述触发字段和请求的数据类型。 */
export interface BuaFormFillRequest {
  readonly triggerFields: readonly BuaActionTarget[];
  readonly requestedData:
      'address'|'shipping_address'|'billing_address'|'home_address'|
      'work_address'|'credit_card'|'contact_information'|'other';
  readonly sectionLabel?: string;
}

/** OTP/验证码填充动作。 */
export interface BuaAttemptOtpFillAction extends BuaBaseAction {
  readonly kind: 'attempt_otp_fill';
  readonly targetFields: readonly BuaActionTarget[];
  readonly forSignin?: boolean;
}

/** 调用 frame/document 暴露的结构化页面工具。 */
export interface BuaCallPageToolAction extends BuaBaseAction {
  readonly kind: 'call_page_tool';
  readonly frameNodeId: BuaId;
  readonly toolName: string;
  readonly arguments?: BuaMetadata;
}

/** 媒体控制动作，例如播放、暂停和跳转。 */
export interface BuaMediaAction extends BuaBaseAction {
  readonly kind: 'media';
  readonly operation: 'play'|'pause'|'seek';
  readonly seekTimeMs?: number;
}

/** 截图或区域捕获动作。 */
export interface BuaCaptureAction extends BuaBaseAction {
  readonly kind: 'capture';
  readonly operation: 'screenshot'|'region';
  readonly target?: BuaActionTarget;
  readonly screenshot?: BuaScreenshotOptions;
}

/** 截图采集参数，用于控制尺寸、格式和压缩质量。 */
export interface BuaScreenshotOptions {
  readonly maxWidth?: number;
  readonly maxHeight?: number;
  readonly format?: 'jpeg'|'png'|'webp';
  readonly quality?: 'low'|'medium'|'high';
}

/** 将控制权交给用户的动作，适合需要人工完成的步骤。 */
export interface BuaYieldToUserAction extends BuaBaseAction {
  readonly kind: 'yield_to_user';
  readonly reason?: string;
}

/** 动作执行选项，用于控制目标、速度/安全模式、超时和动作后快照策略。 */
export interface BuaActOptions {
  target?: BuaTargetRef;
  mode?: 'safe'|'fast';
  timeoutMs?: number;
  snapshotAfter?: 'auto'|'full'|'fast'|'none';
  stopOnFirstError?: boolean;
}

/** 动作执行结果，是业务侧判断下一步、恢复或结束任务的核心结构。 */
export interface BuaActionResult {
  readonly ok: boolean;
  readonly status:
      'succeeded'|'failed'|'cancelled'|'paused'|'needs_user'|'unsupported';
  readonly actionId: BuaId;
  readonly failedActionIndex?: number;

  readonly code: string;
  readonly category?: BuaErrorCategory;
  readonly message?: string;

  readonly effects?: readonly BuaActionEffect[];
  readonly snapshot?: BuaPageSnapshot;
  readonly pendingRequest?: BuaUserRequest;
  readonly recovery?: readonly BuaRecoveryHint[];
  readonly timing?: BuaTiming;
  readonly diagnostics?: readonly BuaDiagnostic[];
}

/** 动作对浏览器状态造成的可观察影响。 */
export type BuaActionEffect =
  | {type: 'page_changed'}
  | {type: 'navigation_started'; url?: string}
  | {type: 'navigation_committed'; url?: string}
  | {type: 'target_created'; target: BuaTargetSnapshot}
  | {type: 'target_closed'; targetId: BuaId}
  | {type: 'user_request'; request: BuaUserRequest}
  | {type: 'download_started'}
  | {type: 'file_picker_opened'};

/** 面向业务和 LLM 恢复逻辑的稳定错误分类。 */
export type BuaErrorCategory =
  | 'target_not_found'
  | 'stale_target'
  | 'visibility'
  | 'input'
  | 'navigation'
  | 'permission_policy'
  | 'browser_state'
  | 'user_intervention'
  | 'task_state'
  | 'timeout'
  | 'unsupported'
  | 'backend_error';

/** 给业务侧或 LLM 的恢复建议，不强制执行。 */
export interface BuaRecoveryHint {
  readonly type:
      'refresh_snapshot'|'try_alternative_target'|'wait'|'scroll'|
      'request_permission'|'respond_to_user_request'|'yield_to_user'|
      'stop_task'|'retry'|'use_screenshot';
  readonly reason: string;
  readonly action?: BuaAction;
}

/** 时间信息，可包含整体耗时和分阶段耗时。 */
export interface BuaTiming {
  readonly startedAtMs?: BuaTimestampMs;
  readonly endedAtMs?: BuaTimestampMs;
  readonly elapsedMs?: number;
  readonly phases?: readonly BuaTimingPhase[];
}

/** 单个执行阶段的耗时。 */
export interface BuaTimingPhase {
  readonly name: string;
  readonly elapsedMs: number;
}

/** 用户请求 API，用于处理凭据选择、确认、文件选择等需要外部响应的流程。 */
export interface BuaUserRequestApi {
  /** 获取下一个待处理用户请求；超时后返回 undefined。 */
  next(options?: {timeoutMs?: number}): Promise<BuaUserRequest|undefined>;
  /** 对指定用户请求给出响应。 */
  respond(requestId: BuaId, response: BuaUserResponse): Promise<void>;
  /** 监听新用户请求，适合业务侧 UI 实时弹窗。 */
  onRequest(handler: (request: BuaUserRequest) => void): BuaSubscription;
}

/** 所有需要用户或业务 UI 介入的请求类型。 */
export type BuaUserRequest =
  | BuaCredentialSelectionRequest
  | BuaAutofillSelectionRequest
  | BuaUserConfirmationRequest
  | BuaNavigationConfirmationRequest
  | BuaFilePickerRequest
  | BuaUserTakeoverRequest;

/** 用户请求的公共字段。 */
export interface BuaBaseUserRequest {
  readonly id: BuaId;
  readonly taskId?: BuaId;
  readonly createdAtMs: BuaTimestampMs;
  readonly message?: string;
}

/** 凭据选择请求，SDK 不保存敏感凭据，只传递可展示的选项元信息。 */
export interface BuaCredentialSelectionRequest extends BuaBaseUserRequest {
  readonly kind: 'credential_selection';
  readonly showDialog: boolean;
  readonly credentials: readonly BuaCredentialOption[];
}

/** 可供用户选择的凭据选项。 */
export interface BuaCredentialOption {
  readonly id: BuaId;
  readonly username?: string;
  readonly sourceSiteOrApp?: string;
  readonly requestOrigin?: string;
  readonly type?: 'password'|'federated'|'unknown';
}

/** Autofill 建议选择请求。 */
export interface BuaAutofillSelectionRequest extends BuaBaseUserRequest {
  readonly kind: 'autofill_selection';
  readonly forms: readonly BuaAutofillFormRequest[];
}

/** 单个表单的 autofill 建议请求。 */
export interface BuaAutofillFormRequest {
  readonly index: number;
  readonly requestedData?: string;
  readonly formattedRequestOrigin?: string;
  readonly sectionLabel?: string;
  readonly suggestions: readonly BuaAutofillSuggestion[];
}

/** 单条 autofill 建议。 */
export interface BuaAutofillSuggestion {
  readonly id: BuaId;
  readonly title: string;
  readonly details?: string;
}

/** 用户确认请求，例如敏感动作或导航前确认。 */
export interface BuaUserConfirmationRequest extends BuaBaseUserRequest {
  readonly kind: 'user_confirmation';
  readonly origin?: string;
  readonly reason?: 'navigation'|'sensitive_action'|'download'|'unknown';
  readonly blockedByRisk?: boolean;
}

/** 导航确认请求，通常用于跨站点或高风险 origin。 */
export interface BuaNavigationConfirmationRequest extends BuaBaseUserRequest {
  readonly kind: 'navigation_confirmation';
  readonly origin: string;
}

/** 文件选择请求，业务侧可接管文件选择 UI。 */
export interface BuaFilePickerRequest extends BuaBaseUserRequest {
  readonly kind: 'file_picker';
  readonly accept?: readonly string[];
  readonly multiple?: boolean;
}

/** 用户接管请求，表示当前步骤更适合人工完成。 */
export interface BuaUserTakeoverRequest extends BuaBaseUserRequest {
  readonly kind: 'user_takeover';
  readonly reason?: string;
}

/** 所有用户请求响应类型。 */
export type BuaUserResponse =
  | BuaCredentialSelectionResponse
  | BuaAutofillSelectionResponse
  | BuaUserConfirmationResponse
  | BuaNavigationConfirmationResponse
  | BuaFilePickerResponse
  | BuaUserTakeoverResponse;

/** 凭据选择响应。 */
export interface BuaCredentialSelectionResponse {
  readonly kind: 'credential_selection';
  readonly selectedCredentialId?: BuaId;
  readonly permissionDuration?: 'one_time'|'always_allow';
}

/** Autofill 选择响应。 */
export interface BuaAutofillSelectionResponse {
  readonly kind: 'autofill_selection';
  readonly selectedSuggestions: readonly {
    readonly formIndex: number;
    readonly suggestionId: BuaId;
  }[];
}

/** 用户确认响应。 */
export interface BuaUserConfirmationResponse {
  readonly kind: 'user_confirmation';
  readonly granted: boolean;
}

/** 导航确认响应。 */
export interface BuaNavigationConfirmationResponse {
  readonly kind: 'navigation_confirmation';
  readonly granted: boolean;
}

/** 文件选择响应。 */
export interface BuaFilePickerResponse {
  readonly kind: 'file_picker';
  readonly files?: readonly BuaFileRef[];
  readonly cancelled?: boolean;
}

/** 文件引用。实现可选择提供路径、二进制数据或仅提供文件元信息。 */
export interface BuaFileRef {
  readonly id?: BuaId;
  readonly name: string;
  readonly path?: string;
  readonly mimeType?: string;
  readonly data?: ArrayBuffer;
}

/** 用户接管完成后的响应。 */
export interface BuaUserTakeoverResponse {
  readonly kind: 'user_takeover';
  readonly completed: boolean;
  readonly message?: string;
}

/** 事件 API，用于订阅任务、目标、观察、动作和诊断等运行时变化。 */
export interface BuaEventApi {
  /** 订阅指定类型事件，返回可取消订阅对象。 */
  on<T extends BuaEventType>(
      type: T, handler: (event: BuaEvent<T>) => void): BuaSubscription;
}

/** BUA runtime 对外发布的事件类型。 */
export type BuaEventType =
  | 'task_state_changed'
  | 'target_changed'
  | 'snapshot_invalidated'
  | 'availability_changed'
  | 'permission_changed'
  | 'action_started'
  | 'action_finished'
  | 'user_request'
  | 'diagnostic';

/** 事件类型到 payload 的映射。 */
export interface BuaEventMap {
  readonly task_state_changed: BuaTaskState;
  readonly target_changed: BuaTargetSnapshot|BuaNoTargetState;
  readonly snapshot_invalidated: BuaSnapshotInvalidatedEvent;
  readonly availability_changed: BuaAvailability;
  readonly permission_changed: BuaPermissionState;
  readonly action_started: BuaActionEvent;
  readonly action_finished: BuaActionResult;
  readonly user_request: BuaUserRequest;
  readonly diagnostic: BuaDiagnostic;
}

/** 事件对象，type 决定 value 的具体类型。 */
export interface BuaEvent<T extends BuaEventType = BuaEventType> {
  readonly type: T;
  readonly createdAtMs: BuaTimestampMs;
  readonly value: BuaEventMap[T];
}

/** 页面快照失效事件，用于提醒业务侧不要继续使用旧 nodeId。 */
export interface BuaSnapshotInvalidatedEvent {
  readonly snapshotId?: BuaId;
  readonly generation?: number;
  readonly reason: string;
}

/** 动作开始事件。 */
export interface BuaActionEvent {
  readonly actionId: BuaId;
  readonly action: BuaAction;
}

/** 订阅句柄，调用 unsubscribe 后不再接收事件。 */
export interface BuaSubscription {
  /** 取消订阅；多次调用应是安全的。 */
  unsubscribe(): void;
}

/** 诊断 API，用于读取 trace、上报诊断信息和监听诊断事件。 */
export interface BuaDiagnosticsApi {
  /** 获取当前 session 的 trace，便于排查失败和评估耗时。 */
  trace(): Promise<BuaTrace>;
  /** 上报一条诊断信息，通常由 runtime 或 adapter 调用。 */
  report(diagnostic: BuaDiagnostic): void;
  /** 监听诊断信息。 */
  onDiagnostic(handler: (diagnostic: BuaDiagnostic) => void): BuaSubscription;
}

/** 一次 session/task 的结构化执行轨迹。 */
export interface BuaTrace {
  readonly sessionId: BuaId;
  readonly taskId?: BuaId;
  readonly startedAtMs?: BuaTimestampMs;
  readonly endedAtMs?: BuaTimestampMs;
  readonly entries: readonly BuaTraceEntry[];
}

/** trace 中的一条记录。 */
export interface BuaTraceEntry {
  readonly id: BuaId;
  readonly type:
      'snapshot'|'act'|'availability'|'target'|'user_request'|'event'|
      'backend'|'diagnostic';
  readonly createdAtMs: BuaTimestampMs;
  readonly message?: string;
  readonly timing?: BuaTiming;
  readonly diagnostic?: BuaDiagnostic;
  readonly metadata?: BuaMetadata;
}

/** 统一诊断信息，backend 原始错误只能放在 backend 字段中。 */
export interface BuaDiagnostic {
  readonly level: 'debug'|'info'|'warning'|'error';
  readonly code: string;
  readonly message: string;
  readonly category?: BuaErrorCategory|'snapshot'|'availability'|'adapter';
  readonly backend?: BuaBackendDiagnostic;
  readonly metadata?: BuaMetadata;
}

/** backend 原始诊断信息，用于排查问题，不作为业务稳定 contract。 */
export interface BuaBackendDiagnostic {
  readonly name?: string;
  readonly code?: string|number;
  readonly message?: string;
}

/***************************************************** */
/* backend adapter interface */
/***************************************************** */

/** backend adapter 边界。实现方负责把通用 BUA 调用翻译到底层浏览器能力。 */
export interface BuaBackendAdapter {
  /** 返回 backend 理论支持的 BUA 能力。 */
  capabilities(): Promise<BuaCapabilities>;
  /** 返回当前运行时可用性；未实现时由 SDK runtime 根据事件和能力推断。 */
  availability?(): Promise<BuaAvailability>;

  /** 启动 backend 原生 task；没有原生 task 的 backend 可以不实现。 */
  startTask?(options?: BuaTaskOptions): Promise<BuaBackendTask>;
  /** 停止 backend 原生 task。 */
  stopTask?(reason?: string): Promise<void>;
  /** 取消 backend 当前动作。 */
  cancelActions?(reason?: string): Promise<void>;

  /** 返回 backend 的目标管理实现。 */
  targets(): BuaBackendTargetApi;
  /** 执行一次底层页面读取，并返回已经归一化的 BUA snapshot。 */
  snapshot(options: BuaSnapshotOptions): Promise<BuaBackendSnapshot>;
  /** 执行动作序列，并返回已经归一化的 BUA action result。 */
  perform(
      actions: readonly BuaBackendAction[],
      options: BuaBackendActOptions): Promise<BuaBackendActionResult>;

  /** 订阅 backend 原始事件，由 SDK runtime 转换为 BUA event。 */
  subscribe?(
      event: BuaBackendEventType,
      handler: BuaBackendEventHandler): BuaSubscription;

  /** 响应 backend 发起的用户请求。 */
  respondToUserRequest?(
      requestId: BuaId, response: BuaUserResponse): Promise<void>;

  /** 关闭 backend 资源。 */
  close(reason?: string): Promise<void>;
}

/** backend task 句柄，native 字段只供 adapter/runtime 内部使用。 */
export interface BuaBackendTask {
  readonly id: BuaId;
  readonly state: BuaTaskState;
  readonly native?: unknown;
}

/** backend 目标管理接口，形状与 public target API 对齐。 */
export interface BuaBackendTargetApi {
  /** 获取 backend 当前目标。 */
  current(): Promise<BuaTargetSnapshot|BuaNoTargetState>;
  /** 列出 backend 当前可见目标。 */
  list(options?: BuaListTargetsOptions): Promise<readonly BuaTargetSnapshot[]>;
  /** 创建 tab。 */
  createTab(options?: BuaCreateTabOptions): Promise<BuaTargetSnapshot>;
  /** 激活目标。 */
  activate(target: BuaTargetRef): Promise<void>;
  /** 关闭目标。 */
  close(target: BuaTargetRef): Promise<void>;
  /** 创建窗口。 */
  createWindow(options?: BuaCreateWindowOptions): Promise<BuaWindowSnapshot>;
  /** 激活窗口。 */
  activateWindow(windowId: BuaId): Promise<void>;
  /** 关闭窗口。 */
  closeWindow(windowId: BuaId): Promise<void>;
}

/** backend 页面快照结果，native 字段用于保存底层原始数据。 */
export interface BuaBackendSnapshot {
  readonly snapshot: BuaPageSnapshot;
  readonly native?: unknown;
}

/** 传给 backend 的动作，nativeTarget 可携带 runtime 解析后的底层目标句柄。 */
export interface BuaBackendAction {
  readonly action: BuaAction;
  readonly nativeTarget?: unknown;
}

/** backend 动作选项，比 public act options 多 taskId 等 runtime 信息。 */
export interface BuaBackendActOptions extends BuaActOptions {
  readonly taskId?: BuaId;
}

/** backend 动作结果，native 字段用于调试和 adapter 内部处理。 */
export interface BuaBackendActionResult {
  readonly result: BuaActionResult;
  readonly native?: unknown;
}

/** backend 事件类型，SDK runtime 会把它们归一化成 BuaEventType。 */
export type BuaBackendEventType =
  | 'task_state'
  | 'target_state'
  | 'availability'
  | 'user_request'
  | 'diagnostic';

/** backend 事件处理函数。 */
export type BuaBackendEventHandler =
    (event: BuaBackendEvent) => void|Promise<void>;

/** backend 原始事件载体。 */
export interface BuaBackendEvent {
  readonly type: BuaBackendEventType;
  readonly createdAtMs: BuaTimestampMs;
  readonly value: unknown;
}
