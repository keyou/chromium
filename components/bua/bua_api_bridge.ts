// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

import type {
  BuaErrorCategory,
  BuaJsonObject,
  BuaJsonValue,
  BuaMetadata,
  BuaSession,
  BuaTaskState,
} from './bua_api2.js';

export const BUA_BRIDGE_TOOL_NAMES = [
  'bua_tab_create',
  'bua_tab_list',
  'bua_tab_current',
  'bua_tab_activate',
  'bua_tab_close',
  'bua_page_navigate',
  'bua_page_back',
  'bua_page_forward',
  'bua_page_reload',
  'bua_page_snapshot',
  'bua_page_screenshot',
  'bua_page_extract_content',
  'bua_page_click',
  'bua_page_type',
  'bua_page_scroll',
  'bua_page_scrollto',
  'bua_page_movemouse',
  'bua_page_drag',
  'bua_page_select',
  'bua_page_wait',
  'bua_take_over',
] as const;

export type BuaBridgeToolName = typeof BUA_BRIDGE_TOOL_NAMES[number];

export type BuaBridgeOutputMode = 'json'|'content'|'mixed';

export interface BuaBridgeToolDefinition {
  readonly name: BuaBridgeToolName;
  readonly description: string;
  readonly inputSchema: BuaBridgeJsonSchema;
  readonly outputMode: BuaBridgeOutputMode;
}

export interface BuaBridgeOptions {
  readonly session: BuaSession;
  // Host-owned user takeover flow; Bridge only pauses and resumes the task.
  readonly takeOverHandler?: BuaTakeOverHandler;
}

export interface BuaBridge {
  // Stable order follows BUA_BRIDGE_TOOL_NAMES.
  tools(): readonly BuaBridgeToolDefinition[];
  // Validates args, dispatches through the injected session, and formats
  // output.
  invokeTool(
      name: BuaBridgeToolName,
      args: BuaBridgeToolArgs,
      context?: BuaBridgeInvokeContext): Promise<BuaBridgeResult>;
}

export type BuaBridgeToolArgs = BuaJsonObject;

export interface BuaBridgeInvokeContext {
  readonly toolCallId?: string;
  readonly metadata?: BuaMetadata;
}

export type BuaBridgeResult<TData = BuaJsonValue> =
    BuaBridgeSuccessResult<TData>|BuaBridgeFailureResult<TData>;

export interface BuaBridgeSuccessResult<TData = BuaJsonValue> {
  readonly ok: true;
  readonly data?: TData;
  readonly content?: BuaBridgeContent;
}

export interface BuaBridgeFailureResult<TData = BuaJsonValue> {
  readonly ok: false;
  readonly data?: TData;
  readonly content?: BuaBridgeContent;
  readonly error: BuaBridgeError;
}

export interface BuaBridgeContent {
  readonly format: 'yaml';
  readonly text: string;
}

export interface BuaBridgeError {
  readonly code: string;
  readonly category: BuaErrorCategory|'bridge';
  readonly message: string;
  readonly details?: BuaJsonObject;
}

export type BuaTakeOverHandler =
    (request: BuaTakeOverRequest) => Promise<BuaTakeOverHandlerResult>;

export interface BuaTakeOverRequest {
  readonly reason?: string;
  readonly toolCallId?: string;
  readonly metadata?: BuaMetadata;
}

export type BuaTakeOverHandlerResult =
    BuaTakeOverHandlerSuccessResult|BuaTakeOverHandlerFailureResult;

export interface BuaTakeOverHandlerSuccessResult {
  readonly ok: true;
  readonly result: BuaTakeOverHandlerData;
}

export interface BuaTakeOverHandlerFailureResult {
  readonly ok: false;
  readonly error: BuaBridgeError;
}

export interface BuaTakeOverHandlerData {
  readonly status: 'completed'|'cancelled'|'failed';
  readonly reason?: string;
  readonly metadata?: BuaMetadata;
}

export interface BuaTakeOverBridgeData {
  readonly status: 'completed'|'cancelled'|'failed';
  readonly pauseState?: BuaTaskState;
  readonly resumeState?: BuaTaskState;
  readonly reason?: string;
}

export type BuaBridgeMethodPath =
    'session.tabs.create'|'session.tabs.list'|'session.tabs.current'|
    'session.tabs.activate'|'session.tabs.close'|'session.page.navigate'|
    'session.page.act'|
    'session.page.snapshot'|'session.page.screenshot'|
    'session.page.snapshot>bridge.extractContent'|
    'session.task.pause>takeOverHandler>session.task.resume';

export const BUA_BRIDGE_TOOL_METHOD_MAP: Record<
    BuaBridgeToolName, BuaBridgeMethodPath> = {
  bua_tab_create: 'session.tabs.create',
  bua_tab_list: 'session.tabs.list',
  bua_tab_current: 'session.tabs.current',
  bua_tab_activate: 'session.tabs.activate',
  bua_tab_close: 'session.tabs.close',
  bua_page_navigate: 'session.page.navigate',
  bua_page_back: 'session.page.act',
  bua_page_forward: 'session.page.act',
  bua_page_reload: 'session.page.act',
  bua_page_snapshot: 'session.page.snapshot',
  bua_page_screenshot: 'session.page.screenshot',
  bua_page_extract_content: 'session.page.snapshot>bridge.extractContent',
  bua_page_click: 'session.page.act',
  bua_page_type: 'session.page.act',
  bua_page_scroll: 'session.page.act',
  bua_page_scrollto: 'session.page.act',
  bua_page_movemouse: 'session.page.act',
  bua_page_drag: 'session.page.act',
  bua_page_select: 'session.page.act',
  bua_page_wait: 'session.page.act',
  bua_take_over: 'session.task.pause>takeOverHandler>session.task.resume',
};

export type BuaBridgeJsonSchema = {
  readonly type?: string|readonly string[];
  readonly description?: string;
  readonly enum?: readonly BuaJsonValue[];
  readonly const?: BuaJsonValue;
  readonly properties?: {[key: string]: BuaBridgeJsonSchema};
  readonly required?: readonly string[];
  readonly additionalProperties?: boolean|BuaBridgeJsonSchema;
  readonly items?: BuaBridgeJsonSchema;
  readonly oneOf?: readonly BuaBridgeJsonSchema[];
  readonly minimum?: number;
  readonly maximum?: number;
};

const EMPTY_INPUT_SCHEMA: BuaBridgeJsonSchema = {
  type: 'object',
  properties: {},
  additionalProperties: false,
};

const TAB_ID_SCHEMA: BuaBridgeJsonSchema = {
  type: 'string',
  description:
      'Tab id returned by bua_tab_list, bua_tab_create, or bua_tab_current.',
};

const POINT_SCHEMA: BuaBridgeJsonSchema = {
  type: 'object',
  description:
      'Coordinate fallback target. Use only when no nodeId is available or the backend only supports coordinates.',
  properties: {
    x: {type: 'number'},
    y: {type: 'number'},
  },
  required: ['x', 'y'],
  additionalProperties: false,
};

const ELEMENT_TARGET_SCHEMA: BuaBridgeJsonSchema = {
  description:
      'Prefer nodeId from bua_page_snapshot when available; use point only as a coordinate fallback.',
  oneOf: [
    {
      type: 'object',
      description: 'Preferred target form.',
      properties: {
        nodeId: {
          type: 'string',
          description:
              'Preferred action target. Use an id returned by bua_page_snapshot.',
        },
      },
      required: ['nodeId'],
      additionalProperties: false,
    },
    {
      type: 'object',
      description: 'Coordinate fallback target form.',
      properties: {point: POINT_SCHEMA},
      required: ['point'],
      additionalProperties: false,
    },
  ],
};

const WAIT_OPTIONS_SCHEMA: BuaBridgeJsonSchema = {
  type: 'object',
  properties: {
    condition: {
      oneOf: [
        {
          type: 'object',
          properties: {
            type: {const: 'time'},
            ms: {type: 'number', minimum: 0},
          },
          required: ['type', 'ms'],
          additionalProperties: false,
        },
        {
          type: 'object',
          properties: {
            type: {const: 'page_stable'},
            stableForMs: {type: 'number', minimum: 0},
          },
          required: ['type'],
          additionalProperties: false,
        },
        {
          type: 'object',
          properties: {
            type: {const: 'url_matches'},
            pattern: {type: 'string'},
          },
          required: ['type', 'pattern'],
          additionalProperties: false,
        },
        {
          type: 'object',
          properties: {
            type: {const: 'text_present'},
            text: {type: 'string'},
          },
          required: ['type', 'text'],
          additionalProperties: false,
        },
        {
          type: 'object',
          properties: {
            type: {const: 'element_present'},
            target: ELEMENT_TARGET_SCHEMA,
          },
          required: ['type', 'target'],
          additionalProperties: false,
        },
        {
          type: 'object',
          properties: {
            type: {const: 'element_absent'},
            target: ELEMENT_TARGET_SCHEMA,
          },
          required: ['type', 'target'],
          additionalProperties: false,
        },
      ],
    },
    timeoutMs: {type: 'number', minimum: 0},
  },
  required: ['condition'],
  additionalProperties: false,
};

export const BUA_BRIDGE_TOOL_DEFINITIONS: Record<
    BuaBridgeToolName, BuaBridgeToolDefinition> = {
  bua_tab_create: {
    name: 'bua_tab_create',
    description: 'Open a new browser tab and optionally make it current.',
    inputSchema: {
      type: 'object',
      properties: {
        url: {type: 'string'},
        activate: {type: 'boolean'},
      },
      additionalProperties: false,
    },
    outputMode: 'json',
  },
  bua_tab_list: {
    name: 'bua_tab_list',
    description: 'List browser tabs visible to the current BUA session.',
    inputSchema: EMPTY_INPUT_SCHEMA,
    outputMode: 'json',
  },
  bua_tab_current: {
    name: 'bua_tab_current',
    description: 'Return the current tab used by page tools.',
    inputSchema: EMPTY_INPUT_SCHEMA,
    outputMode: 'json',
  },
  bua_tab_activate: {
    name: 'bua_tab_activate',
    description: 'Make a tab current for subsequent page tools.',
    inputSchema: {
      type: 'object',
      properties: {tabId: TAB_ID_SCHEMA},
      required: ['tabId'],
      additionalProperties: false,
    },
    outputMode: 'json',
  },
  bua_tab_close: {
    name: 'bua_tab_close',
    description: 'Close a browser tab.',
    inputSchema: {
      type: 'object',
      properties: {tabId: TAB_ID_SCHEMA},
      required: ['tabId'],
      additionalProperties: false,
    },
    outputMode: 'json',
  },
  bua_page_navigate: {
    name: 'bua_page_navigate',
    description: 'Navigate the current page to a URL.',
    inputSchema: {
      type: 'object',
      properties: {
        url: {type: 'string'},
      },
      required: ['url'],
      additionalProperties: false,
    },
    outputMode: 'mixed',
  },
  bua_page_back: {
    name: 'bua_page_back',
    description: 'Navigate the current page back in history.',
    inputSchema: EMPTY_INPUT_SCHEMA,
    outputMode: 'mixed',
  },
  bua_page_forward: {
    name: 'bua_page_forward',
    description: 'Navigate the current page forward in history.',
    inputSchema: EMPTY_INPUT_SCHEMA,
    outputMode: 'mixed',
  },
  bua_page_reload: {
    name: 'bua_page_reload',
    description: 'Reload the current page.',
    inputSchema: {
      type: 'object',
      properties: {
        ignoreCache: {type: 'boolean'},
      },
      additionalProperties: false,
    },
    outputMode: 'mixed',
  },
  bua_page_snapshot: {
    name: 'bua_page_snapshot',
    description: 'Read a structured snapshot of the current page.',
    inputSchema: {
      type: 'object',
      properties: {
        purpose: {enum: ['observe', 'act', 'recover']},
        includeScreenshot: {type: 'boolean'},
        maxNodes: {type: 'number', minimum: 0},
      },
      additionalProperties: false,
    },
    outputMode: 'mixed',
  },
  bua_page_screenshot: {
    name: 'bua_page_screenshot',
    description: 'Capture a screenshot of the current page.',
    inputSchema: {
      type: 'object',
      properties: {
        format: {enum: ['png', 'jpeg']},
        fullPage: {type: 'boolean'},
      },
      additionalProperties: false,
    },
    outputMode: 'json',
  },
  bua_page_extract_content: {
    name: 'bua_page_extract_content',
    description: 'Extract readable content from the current page.',
    inputSchema: EMPTY_INPUT_SCHEMA,
    outputMode: 'content',
  },
  bua_page_click: {
    name: 'bua_page_click',
    description:
        'Click a nodeId target from snapshot, or a point fallback.',
    inputSchema: {
      type: 'object',
      properties: {
        target: ELEMENT_TARGET_SCHEMA,
        button: {enum: ['left', 'right']},
        clickCount: {type: 'number', minimum: 1},
      },
      required: ['target'],
      additionalProperties: false,
    },
    outputMode: 'mixed',
  },
  bua_page_type: {
    name: 'bua_page_type',
    description:
        'Type text into a nodeId target from snapshot, or a point fallback.',
    inputSchema: {
      type: 'object',
      properties: {
        target: ELEMENT_TARGET_SCHEMA,
        text: {type: 'string'},
        replace: {type: 'boolean'},
        submit: {type: 'boolean'},
      },
      required: ['target', 'text'],
      additionalProperties: false,
    },
    outputMode: 'mixed',
  },
  bua_page_scroll: {
    name: 'bua_page_scroll',
    description: 'Scroll a page or scrollable region by a delta or direction.',
    inputSchema: {
      type: 'object',
      properties: {
        target: ELEMENT_TARGET_SCHEMA,
        deltaX: {type: 'number'},
        deltaY: {type: 'number'},
        direction: {enum: ['up', 'down', 'left', 'right']},
        amount: {type: 'number', minimum: 0},
      },
      required: ['target'],
      additionalProperties: false,
    },
    outputMode: 'mixed',
  },
  bua_page_scrollto: {
    name: 'bua_page_scrollto',
    description:
        'Scroll the current page to a nodeId target, or a point fallback.',
    inputSchema: {
      type: 'object',
      properties: {target: ELEMENT_TARGET_SCHEMA},
      required: ['target'],
      additionalProperties: false,
    },
    outputMode: 'mixed',
  },
  bua_page_movemouse: {
    name: 'bua_page_movemouse',
    description:
        'Move the pointer to a nodeId target, or a point fallback.',
    inputSchema: {
      type: 'object',
      properties: {target: ELEMENT_TARGET_SCHEMA},
      required: ['target'],
      additionalProperties: false,
    },
    outputMode: 'mixed',
  },
  bua_page_drag: {
    name: 'bua_page_drag',
    description:
        'Drag between nodeId targets from snapshot, or point fallbacks.',
    inputSchema: {
      type: 'object',
      properties: {
        from: ELEMENT_TARGET_SCHEMA,
        to: ELEMENT_TARGET_SCHEMA,
      },
      required: ['from', 'to'],
      additionalProperties: false,
    },
    outputMode: 'mixed',
  },
  bua_page_select: {
    name: 'bua_page_select',
    description:
        'Select option values in a nodeId target from snapshot, or a point fallback.',
    inputSchema: {
      type: 'object',
      properties: {
        target: ELEMENT_TARGET_SCHEMA,
        values: {
          type: 'array',
          items: {type: 'string'},
        },
      },
      required: ['target', 'values'],
      additionalProperties: false,
    },
    outputMode: 'mixed',
  },
  bua_page_wait: {
    name: 'bua_page_wait',
    description:
        'Wait for time, page stability, URL, text, element presence, or element absence.',
    inputSchema: WAIT_OPTIONS_SCHEMA,
    outputMode: 'mixed',
  },
  bua_take_over: {
    name: 'bua_take_over',
    description: 'Pause the task and let the user take over the browser.',
    inputSchema: {
      type: 'object',
      properties: {
        reason: {type: 'string'},
      },
      additionalProperties: false,
    },
    outputMode: 'json',
  },
};
