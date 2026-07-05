# BUA API2 Implementation Plan

## Goal

- Use `components/bua/bua_api2.ts` as the single public browser-use contract.
- Provide `bua-server3` as the API2 demo application backed by the real
  Chromium/Glic runtime, with no mock browser implementation.
- Keep the LLM bridge provider-neutral and expose only the core tool surface in
  `components/bua/bua_api_bridge.ts`.
- Remove legacy API surface area from the active product path instead of
  preserving compatibility adapters.

## Contract Shape

The browser global is:

- `bua.capabilities()`
- `bua.createSession(options?)`

Each session exposes:

- `task.start/state/pause/resume/cancel/stop`
- `tabs.create/list/current/activate/close`
- `page.navigate/snapshot/screenshot/act`
- `events.subscribe`
- `close`

Page content returned to LLM callers is fixed to YAML. External callers cannot
choose Markdown or another text format.

## Bridge Tool Surface

Bridge tools remain the 21 core tools:

- Tabs: `bua_tab_create`, `bua_tab_list`, `bua_tab_current`,
  `bua_tab_activate`, `bua_tab_close`
- Navigation: `bua_page_navigate`, `bua_page_back`, `bua_page_forward`,
  `bua_page_reload`
- Sensing: `bua_page_snapshot`, `bua_page_screenshot`,
  `bua_page_extract_content`
- Actions: `bua_page_click`, `bua_page_type`, `bua_page_scroll`,
  `bua_page_scrollto`, `bua_page_movemouse`, `bua_page_drag`,
  `bua_page_select`, `bua_page_wait`
- Handoff: `bua_take_over`

`bua_page_extract_content` always calls `session.page.snapshot()` and returns
YAML content.

Action targets follow Glic/Actor's target model. When a caller derives a target
from a page observation, it should prefer the `nodeId` returned by
`bua_page_snapshot`. Native BUA exposes snapshot node ids as
`<document_id>.<dom_node_id>` and keeps the long Glic document identifier mapped
inside the C++ session. `point` is only a coordinate fallback for cases where no
usable node id exists or the backend only supports coordinates. The Bridge must
not accept `text` or `selector` targets or run a hidden snapshot before act to
resolve them.

## Native Runtime Mapping

Renderer facade calls native method names that match API2:

- `tabs.create/list/current/activate/close`
- `page.navigate/snapshot/screenshot/act`
- `task.start/state/pause/resume/cancelActions/stop`

The browser dispatcher owns translation from API2 action fields to Chromium
actor backend details, for example `move_mouse`, `clickCount`, `values`, and
wait conditions.

## `bua-server3`

`bua-server3` should:

- Connect only to the real `window.bua` runtime.
- Wrap API2 enough to normalize errors and keep the demo app resilient.
- Register and invoke Bridge tools through `createApi2Bridge`.
- Display capabilities, tab state, page snapshots, screenshots, and task state.
- Avoid mock browser behavior in app code and tests.

## Implementation Sequence

1. Keep API2 and Bridge contracts type-checked and aligned.
2. Keep renderer facade API2-only and test it by executing the injected script
   against a fake native transport.
3. Keep browser dispatcher API2 method names wired to real tab, snapshot, actor,
   and task operations.
4. Expand unsupported API2 waits (`page_stable`, `url_matches`, `text_present`,
   `element_present`, `element_absent`) into product-level browser waits.
5. Wire `bua-server3` UI to exercise every Bridge tool, not only snapshot and
   diagnostic operations.
6. Validate through the real Glic sidebar flow, not by opening `chrome://glic`
   directly as a tab.

## Verification

Run narrow checks while iterating:

```sh
node bua-server3/test/api2_adapter_test.mjs
node bua-server3/test/api2_bridge_test.mjs
node bua-server3/test/renderer_facade_script_test.mjs
node bua-server3/test/native_dispatcher_contract_test.mjs
node bua-server3/test/server3_ui_test.mjs
node --check bua-server3/src/api2/native_api2_client.js
node --check bua-server3/src/api2/api2_bridge.js
node --check bua-server3/main.js
python3 -B -c "import pathlib; [compile(pathlib.Path(p).read_text(), p, 'exec') for p in ('bua-server3/index.py','bua-server3/server.py')]"
node third_party/node/node_modules/typescript/lib/tsc.js --noEmit --module NodeNext --moduleResolution NodeNext --target ES2022 --strict --skipLibCheck components/bua/bua_api2.ts components/bua/bua_api_bridge.ts
PATH="/Users/keyou/dev/depot_tools:$PATH" autoninja -C out/mainr chrome/browser/bua:bua content/renderer:renderer
```

Before claiming completion, also run a Glic sidebar validation with
`bua-server3` loaded as the guest URL.
