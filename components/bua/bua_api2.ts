// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

export type BuaId = string;
export type BuaTimestampMs = number;
export type BuaJsonPrimitive = string|number|boolean|null;
export type BuaJsonValue =
    BuaJsonPrimitive|BuaJsonObject|readonly BuaJsonValue[];
export type BuaJsonObject = {[key: string]: BuaJsonValue};
export type BuaMetadata = Record<string, unknown>;

export interface BuaError {
  readonly code: string;
  readonly category: BuaErrorCategory;
  readonly message: string;
  readonly details?: BuaJsonObject;
}

export type BuaErrorCategory =
    'validation'|'unsupported'|'permission'|'session_closed'|'task_state'|
    'tab_state'|'page_state'|'navigation'|'input'|'timeout'|
    'user_intervention'|'backend'|'unknown';

export interface BuaClient {
  capabilities(): Promise<BuaCapabilities>;
  createSession(options?: BuaSessionOptions): Promise<BuaSession>;
}

export interface BuaSession {
  readonly id: BuaId;
  readonly capabilities: BuaCapabilities;
  readonly task: BuaTask;
  readonly tabs: BuaTabs;
  readonly page: BuaPage;
  readonly events: BuaEvents;

  close(reason?: string): Promise<void>;
}

export interface BuaSessionOptions {
  readonly id?: BuaId;
  readonly metadata?: BuaMetadata;
}

export interface BuaCapabilities {
  readonly backend: BuaBackendInfo;
  readonly task: BuaTaskCapabilities;
  readonly tabs: BuaTabCapabilities;
  readonly page: BuaPageCapabilities;
  readonly events: readonly BuaEventType[];
}

export interface BuaBackendInfo {
  readonly name: string;
  readonly version?: string;
  readonly protocol?: string;
}

export interface BuaTaskCapabilities {
  readonly start: boolean;
  readonly pause: boolean;
  readonly resume: boolean;
  readonly cancel: boolean;
  readonly stop: boolean;
}

export interface BuaTabCapabilities {
  readonly create: boolean;
  readonly list: boolean;
  readonly current: boolean;
  readonly activate: boolean;
  readonly close: boolean;
}

export interface BuaPageCapabilities {
  readonly navigate: boolean;
  readonly snapshot: boolean;
  readonly screenshot: boolean;
  readonly act: boolean;
  readonly actions: readonly BuaActionKind[];
}

export type BuaActionKind =
    'history'|'click'|'type'|'scroll'|'scroll_to'|'move_mouse'|'drag'|
    'select'|'wait';

export interface BuaTask {
  start(options?: BuaTaskStartOptions): Promise<BuaTaskState>;
  state(): Promise<BuaTaskState>;
  pause(reason?: string): Promise<BuaTaskState>;
  resume(snapshot?: boolean|BuaSnapshotOptions): Promise<BuaTaskResumeResult>;
  cancel(reason?: string): Promise<BuaTaskCancelResult>;
  stop(reason?: BuaStopReason): Promise<BuaTaskState>;
}

export interface BuaTaskResumeResult {
  readonly state: BuaTaskState;
  readonly snapshot?: BuaPageSnapshot;
}

export interface BuaTaskCancelResult {
  readonly status: 'cancelled'|'nothing_to_cancel';
  readonly reason?: string;
}

export interface BuaTaskStartOptions {
  readonly title?: string;
  readonly userGoal?: string;
  readonly timeoutMs?: number;
  readonly metadata?: BuaMetadata;
}

export type BuaTaskStatus = 'idle'|'running'|'paused'|'stopped';

export interface BuaTaskState {
  readonly id: BuaId;
  readonly status: BuaTaskStatus;
  readonly startedAtMs?: BuaTimestampMs;
  readonly updatedAtMs: BuaTimestampMs;
  readonly reason?: string;
}

export type BuaStopReason =
    'completed'|'cancelled'|'failed'|'user_requested'|'session_closed'|'other';

export interface BuaTabs {
  create(options?: BuaCreateTabOptions): Promise<BuaTab>;
  list(includeClosed?: boolean): Promise<readonly BuaTab[]>;
  current(): Promise<BuaTab>;
  activate(tabId: BuaId): Promise<BuaTab>;
  close(tabId: BuaId): Promise<boolean>;
}

export interface BuaCreateTabOptions {
  readonly url?: string;
  readonly activate?: boolean;
}

export interface BuaTab {
  readonly id: BuaId;
  readonly title?: string;
  readonly url?: string;
  readonly active: boolean;
  readonly loading?: boolean;
}

export interface BuaPage {
  navigate(url: string): Promise<BuaNavigateResult>;
  snapshot(options?: BuaSnapshotOptions): Promise<BuaPageSnapshot>;
  screenshot(options?: BuaScreenshotOptions): Promise<BuaScreenshot>;
  act(actions: BuaAction|readonly BuaAction[], options?: BuaActOptions):
      Promise<BuaActResult>;
}

export interface BuaNavigateResult {
  readonly ok: boolean;
  readonly requestedUrl: string;
  readonly finalUrl?: string;
  readonly error?: BuaError;
  readonly snapshot?: BuaPageSnapshot;
}

export interface BuaSnapshotOptions {
  readonly purpose?: 'observe'|'act'|'recover';
  readonly includeScreenshot?: boolean;
  readonly maxNodes?: number;
}

export interface BuaPageSnapshot {
  readonly url: string;
  readonly title?: string;
  readonly capturedAtMs: BuaTimestampMs;
  readonly viewport?: BuaViewport;
  readonly content?: BuaPageNode;
  readonly text?: string;
  readonly screenshot?: BuaScreenshot;
}

export interface BuaPageNode {
  readonly id: BuaId;
  readonly role?: string;
  readonly name?: string;
  readonly text?: string;
  readonly value?: string;
  readonly selector?: string;
  readonly bounds?: BuaRect;
  readonly children?: readonly BuaPageNode[];
}

export interface BuaViewport {
  readonly width: number;
  readonly height: number;
  readonly deviceScaleFactor?: number;
}

export interface BuaRect {
  readonly x: number;
  readonly y: number;
  readonly width: number;
  readonly height: number;
}

export interface BuaPoint {
  readonly x: number;
  readonly y: number;
}

export interface BuaScreenshotOptions {
  readonly format?: 'png'|'jpeg';
  readonly fullPage?: boolean;
}

export interface BuaScreenshot {
  readonly mimeType: 'image/png'|'image/jpeg';
  readonly dataBase64: string;
  readonly width?: number;
  readonly height?: number;
}

export type BuaElementTarget =
    // Prefer `nodeId` returned by snapshot(); use `point` only as a coordinate
    // fallback. Local Chromium also accepts "viewport" for the root scroller in
    // scroll actions.
    {readonly nodeId: BuaId}|
    {readonly point: BuaPoint};

export type BuaAction =
    BuaHistoryAction|BuaClickAction|BuaTypeAction|BuaScrollAction|
    BuaScrollToAction|BuaMoveMouseAction|BuaDragAction|BuaSelectAction|
    BuaWaitAction;

export interface BuaBaseAction {
  readonly id?: BuaId;
  readonly timeoutMs?: number;
  readonly metadata?: BuaMetadata;
}

export interface BuaHistoryAction extends BuaBaseAction {
  readonly kind: 'history';
  readonly direction: 'back'|'forward'|'reload';
  readonly ignoreCache?: boolean;
}

export interface BuaClickAction extends BuaBaseAction {
  readonly kind: 'click';
  readonly target: BuaElementTarget;
  readonly button?: 'left'|'right';
  readonly clickCount?: number;
}

export interface BuaTypeAction extends BuaBaseAction {
  readonly kind: 'type';
  readonly target: BuaElementTarget;
  readonly text: string;
  readonly replace?: boolean;
  readonly submit?: boolean;
}

export interface BuaScrollAction extends BuaBaseAction {
  readonly kind: 'scroll';
  // Required so runtimes can disambiguate nested scrollable regions.
  readonly target: BuaElementTarget;
  readonly deltaX?: number;
  readonly deltaY?: number;
  readonly direction?: 'up'|'down'|'left'|'right';
  readonly amount?: number;
}

export interface BuaScrollToAction extends BuaBaseAction {
  readonly kind: 'scroll_to';
  readonly target: BuaElementTarget;
}

export interface BuaMoveMouseAction extends BuaBaseAction {
  readonly kind: 'move_mouse';
  readonly target: BuaElementTarget;
}

export interface BuaDragAction extends BuaBaseAction {
  readonly kind: 'drag';
  readonly from: BuaElementTarget;
  readonly to: BuaElementTarget;
}

export interface BuaSelectAction extends BuaBaseAction {
  readonly kind: 'select';
  readonly target: BuaElementTarget;
  readonly values: readonly string[];
}

export interface BuaWaitAction extends BuaBaseAction {
  readonly kind: 'wait';
  readonly condition: BuaWaitCondition;
  readonly timeoutMs?: number;
}

export type BuaWaitCondition =
    {readonly type: 'time'; readonly ms: number}|
    {readonly type: 'page_stable'; readonly stableForMs?: number}|
    {readonly type: 'url_matches'; readonly pattern: string}|
    {readonly type: 'text_present'; readonly text: string}|
    {readonly type: 'element_present'; readonly target: BuaElementTarget}|
    {readonly type: 'element_absent'; readonly target: BuaElementTarget};

export interface BuaActOptions {
  readonly timeoutMs?: number;
  readonly snapshotAfter?: 'auto'|'full'|'none';
  readonly stopOnFirstError?: boolean;
}

export interface BuaActResult {
  readonly ok: boolean;
  readonly error?: BuaError;
  readonly effects?: readonly BuaActionEffect[];
  readonly snapshot?: BuaPageSnapshot;
}

export type BuaActionEffect =
    {readonly type: 'page_changed'}|
    {readonly type: 'navigation_started'; readonly url?: string}|
    {readonly type: 'navigation_committed'; readonly url?: string}|
    {readonly type: 'download_started'}|
    {readonly type: 'dialog_opened'};

export interface BuaEvents {
  subscribe(
      type: BuaEventType, handler: (event: BuaEvent) => void):
      Promise<BuaSubscription>;
}

export interface BuaSubscription {
  unsubscribe(): Promise<void>;
}

export type BuaEventType =
    'task_state_changed'|'tab_changed'|'page_changed'|'user_takeover';

export type BuaEvent =
    {readonly type: 'task_state_changed'; readonly timestampMs: BuaTimestampMs;
     readonly value: BuaTaskState}|
    {readonly type: 'tab_changed'; readonly timestampMs: BuaTimestampMs;
     readonly value: BuaTab}|
    {readonly type: 'page_changed'; readonly timestampMs: BuaTimestampMs;
     readonly value: BuaPageSnapshot}|
    {readonly type: 'user_takeover'; readonly timestampMs: BuaTimestampMs;
     readonly value: BuaUserTakeoverEvent};

export interface BuaUserTakeoverEvent {
  readonly reason?: string;
  readonly metadata?: BuaMetadata;
}
