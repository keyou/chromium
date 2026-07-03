// 1. SDK/runtime 创建入口
const client = LarkAPI.bua.createBuaClient(adapter);

// 2. 查询 backend 静态能力
const capabilities = await client.capabilities();

// 3. 创建一个 session，开启一个对话
const session = await client.createSession({
  metadata: {goal: '搜索 Chromium BUA'},
  defaultSnapshot: {
    mode: 'interact',
    purpose: 'plan',
    channels: {
      content: capabilities.snapshot.content,
      screenshot: capabilities.snapshot.screenshot,
    },
  },
});

// 4. 注册事件监听
const taskSub = session.on('task_state_changed', event => {
  console.log('task state changed', event.value.status);
});

const actionSub = session.on('action_finished', event => {
  console.log('action finished', event.value.status);
});

// 处理用户请求，要求用户接管
const userRequestSub = session.on('user_request', async event => {
  const request = event.value;

  if (request.kind === 'user_confirmation') {
    await session.respondToUserRequest(request.id, {
      kind: 'user_confirmation',
      granted: true,
    });
    return;
  }

  if (request.kind === 'file_picker') {
    await session.respondToUserRequest(request.id, {
      kind: 'file_picker',
      files: [],
      cancelled: true,
    });
    return;
  }
});

// 5. 获取或创建目标页面
const currentTarget = await session.targets.current();

const target = currentTarget.kind === 'no_target'
  ? await session.targets.createTab({url: 'https://example.com'})
  : currentTarget;

// 6. 激活目标
await session.targets.activate({targetId: target.id});

// 7. 启动 agent task
const taskState = await session.task.start({
  title: 'Search task',
  userGoal: '搜索 Chromium BUA',
  target: {targetId: target.id},
  timeoutMs: 120000,
});

// 8. 如需跳转，使用 act 执行 navigate
const navigateResult = await session.act(
  {kind: 'navigate', url: 'https://example.com'},
  {
    target: {targetId: target.id},
    snapshotAfter: 'fast',
  },
);

// 9. 使用 action 后返回的 snapshot，或读取 session 缓存的 latest snapshot
const snapshot = navigateResult.snapshot ?? session.latestSnapshot();

// 10. 也可以主动读取页面快照
snapshot = await session.snapshot({
  target: {targetId: target.id},
  mode: 'interact',
  purpose: 'plan',
  channels: {
    content: true,
    screenshot: true,
  },
});

// 11. 模拟从 snapshot 中选择要操作的节点，一般由 LLM 选择
const searchBoxNodeId = snapshot.content?.children?.[0]?.id ?? 'mock-node-id';

// 12. 执行动作序列，一般由 LLM 生成
const actionResult = await session.act(
  [
    {
      kind: 'type',
      target: {nodeId: searchBoxNodeId},
      text: 'Chromium BUA',
      replace: true,
      submit: true,
    },
    {
      kind: 'wait',
      waitMs: 500,
    },
  ],
  {
    target: {targetId: target.id},
    mode: 'safe',
    timeoutMs: 15000,
    snapshotAfter: 'auto',
    stopOnFirstError: true,
  },
);

// 13. 根据动作结果决定任务终态
const taskStopReason = actionResult.ok ? 'completed' : 'failed';

// 从动作结果中取出页面快照
const snapshot = actionResult.snapshot;

// 14. 暂停任务，控制权交给用户
await session.task.pause('needs_external_action');

// 15. 恢复任务
const resumedState = await session.task.resume({
snapshot: {
    purpose: 'recover',
    channels: {content: true},
},
});

// 获取恢复后的页面快照
const resumedSnapshot = resumedState.snapshot;

// 16. 取消任务
await session.task.cancel();

// 17. 结束任务
const stopResult = await session.task.stop(taskStopReason);

// 18. 取消订阅
taskSub.unsubscribe();
actionSub.unsubscribe();
userRequestSub.unsubscribe();

// 19. 关闭 session，会释放所有资源
await session.close('task_finished');
