import type { AppState, Task } from './schema.ts';
import { conversationKey as key, requesterKey } from './schema.ts';

export const HOME = key('desktop', 'home');
export function createInitialState(now = Date.now()): AppState {
  const local = key('desktop', 'reading');
  const telegram = key('telegram-main', 'xu-ning');
  const qq = key('qq-main', 'zhou-ran');
  const qqOther = key('qq-studio', 'zhou-ran');
  const feishu = key('feishu-main', 'lin-xiao');
  const conversation = (connectionId: string, locator: string, name: string, unread = 0) => ({
    id: key(connectionId, locator),
    connectionId,
    locator,
    name,
    requesterId: requesterKey(connectionId, connectionId === 'desktop' ? 'you' : locator),
    sessionId: `${connectionId}:${locator}`,
    origin: connectionId === 'desktop' ? ('desktop' as const) : ('channel' as const),
    unread,
  });
  const conversations = [
    conversation('desktop', 'home', '和 Orangutan 聊聊'),
    conversation('desktop', 'reading', '我的阅读角落'),
    conversation('telegram-main', 'xu-ning', '许宁', 1),
    conversation('qq-main', 'zhou-ran', '周然', 2),
    conversation('qq-studio', 'zhou-ran', '周然 · 工作'),
    conversation('feishu-main', 'lin-xiao', '林晓'),
  ];
  const task = (
    id: string,
    cid: string,
    title: string,
    status: Task['status'],
    kind: Task['kind'],
  ): Task => ({
    id,
    conversationId: cid,
    requesterId: conversations.find((c) => c.id === cid)!.requesterId,
    sessionId: conversations.find((c) => c.id === cid)!.sessionId,
    title,
    status,
    kind,
    attempt: 1,
    step: status === 'completed' ? 10 : 0,
    createdAt: now - 900_000,
    language: 'zh',
    shouldFail: false,
    history: [],
    members:
      kind === 'team'
        ? [
            {
              id: 'explorer',
              name: 'Scout',
              role: '资料搜集',
              state: 'done',
              result: '整理了 6 份参考资料。',
            },
            {
              id: 'planner',
              name: 'Moss',
              role: '分析与规划',
              state: status === 'completed' ? 'done' : 'working',
              result: '对比时间与路线，保留两个方案。',
            },
            {
              id: 'reviewer',
              name: 'Fern',
              role: '检查与汇总',
              state: status === 'completed' ? 'done' : 'waiting',
              result: '完成了交叉检查。',
            },
          ]
        : [],
    approval: null,
    artifact: null,
  });
  const reading = task('reading-plan', local, '给阅读留一点时间', 'completed', 'file');
  reading.artifact = {
    name: 'a-small-reading-plan.md',
    kind: 'markdown',
    body: '# 给阅读留一点时间\n\n不需要读得很快，只需要留一点固定的时间。\n\n## 这个月，读两本就好\n\n| 书目 | 适合的时刻 | 节奏 |\n| --- | --- | --- |\n| 《看不见的城市》 | 睡前，放下手机以后 | 每天一座城 |\n| 《植物知道生命的答案》 | 周末的午后 | 每周两章 |\n\n## 一个小小的习惯\n\n- [ ] 把正在读的书放在触手可及的地方\n- [ ] 读到喜欢的一句，就记下来\n- [ ] 周日花十分钟回看摘录\n\n> 不用急着读完。遇到喜欢的一页，多停留一会儿。\n\n来源：演示工作区 · 个人阅读笔记。',
  };
  const travel = task('weekend-plan', telegram, '找一个周末可以散步的地方', 'completed', 'team');
  travel.artifact = {
    name: 'a-slower-weekend.md',
    kind: 'markdown',
    body: '# 把周末过慢一点\n\n## 推荐：沿河走一走\n\n上午出发，带上水和一本书。不赶路，也不安排太多目的地。\n\n1. 10:00 · 河边步道，散步 40 分钟\n2. 11:00 · 找一家有阳光的咖啡店\n3. 14:00 · 附近的独立书店，随意逛逛\n\n## 出门前\n\n- [ ] 确认当天的天气\n- [ ] 带一件薄外套\n\n> Scout 整理地点，Moss 规划节奏，Fern 检查了路线。\n\n来源：演示资料，不是实时地点或天气查询。',
  };
  const files = task('qq-files', qq, '整理一下共享资料里的会议笔记', 'awaiting_approval', 'file');
  files.step = 3;
  files.approval = {
    id: 'qq-files:1:approval',
    status: 'pending',
    target: 'workspace/notes/meeting-summary.md',
  };
  const research = task('studio-notes', qqOther, '对比两种工作记录方式', 'queued', 'team');
  research.members = research.members.map((m) => ({ ...m, state: 'waiting', result: '' }));
  const review = task('feishu-review', feishu, '看看这份活动安排还有什么遗漏', 'failed', 'team');
  review.shouldFail = true;
  review.members[0].state = 'failed';
  review.members[0].result = '示例参考文件暂时不可用。';
  const message = (id: string, cid: string, role: 'user' | 'assistant', text: string) => ({
    id,
    conversationId: cid,
    sessionId: conversations.find((c) => c.id === cid)!.sessionId,
    role,
    text,
    createdAt: now - 800_000,
    state: 'complete' as const,
  });
  return {
    version: 1,
    settings: {
      language: 'zh',
      theme: 'system',
      notifications: true,
      memory: true,
      model: 'Claude Sonnet',
      permission: 'ask',
    },
    selectedId: HOME,
    drafts: {},
    conversations,
    connections: [
      {
        id: 'telegram-main',
        platform: 'telegram',
        name: 'Telegram',
        account: '@orangutan_demo',
        status: 'connecting',
        attempt: 1,
        simulateFailure: false,
      },
      {
        id: 'qq-main',
        platform: 'qq',
        name: 'QQ',
        account: 'Orangutan · 日常',
        status: 'connecting',
        attempt: 1,
        simulateFailure: false,
      },
      {
        id: 'feishu-main',
        platform: 'feishu',
        name: '飞书',
        account: 'Orangutan · 工作空间',
        status: 'disconnected',
        attempt: 0,
        simulateFailure: false,
      },
      {
        id: 'qq-studio',
        platform: 'qq',
        name: 'QQ · 工作',
        account: 'Orangutan · 工作',
        status: 'disconnected',
        attempt: 0,
        simulateFailure: false,
      },
    ],
    tasks: [files, travel, reading, research, review],
    messages: [
      message(
        'read-user',
        local,
        'user',
        '想重新开始读书，但不想把它变成另一项任务。帮我安排得轻松一点。',
      ),
      message(
        'read-assistant',
        local,
        'assistant',
        '那我们就从每天十分钟开始。\n\n选了两本适合慢慢读的书，也留了一些完全不用安排的空白。阅读计划已经整理好，可以在右侧看看。',
      ),
      message(
        'tg-user',
        telegram,
        'user',
        '周末想出去走走，别太赶，帮我看看有什么轻松一点的安排。',
      ),
      message(
        'tg-assistant',
        telegram,
        'assistant',
        '小队给你整理了一份慢节奏的周末安排。\n\n沿河散步、喝杯咖啡，再去附近的书店看看。具体路线和出门前的清单都在方案里。',
      ),
      message('qq-user', qq, 'user', '共享文件夹里有几份会议记录，可以整理成一份吗？'),
      message(
        'qq-assistant',
        qq,
        'assistant',
        '已经看过示例资料，准备写入一份合并后的会议笔记。\n\n这个操作需要在 QQ 中确认，原始记录会保留。',
      ),
      message('qq-other-user', qqOther, 'user', '帮我比较日记式和任务式两种工作记录方式。'),
      message(
        'qq-other-assistant',
        qqOther,
        'assistant',
        '我会让小队比较两种方式，看看各自适合什么样的记录场景。',
      ),
      message('fs-user', feishu, 'user', '帮我检查一下周五活动安排，尤其是物料和时间。'),
      message(
        'fs-assistant',
        feishu,
        'assistant',
        '一份示例参考文件暂时不可用，检查尚未完成。可以在飞书会话中补充资料后重新发起。',
      ),
    ],
    notes: [
      {
        id: 'shared-workspace',
        scope: 'shared',
        ownerId: null,
        title: '让资料各归其位',
        body: '工作资料放在 workspace/projects，阅读与灵感放在 workspace/notes。整理时保留原始文件，先展示方案，再执行变更。',
        source: '共享工作区约定',
        updatedAt: now - 86_400_000,
      },
      {
        id: 'shared-style',
        scope: 'shared',
        ownerId: null,
        title: '说清楚，少一点术语',
        body: '公共文档使用简洁的语言。先说明结论，再给出必要的依据；不确定的内容明确标注。',
        source: '公共写作约定',
        updatedAt: now - 172_800_000,
      },
      {
        id: 'your-reading',
        scope: 'personal',
        ownerId: requesterKey('desktop', 'you'),
        title: '阅读是一段留白',
        body: '希望阅读安排轻松一些，每天十分钟即可，不把读书变成需要打卡的任务。',
        source: '我的阅读角落',
        updatedAt: now - 800_000,
      },
      {
        id: 'tg-preference',
        scope: 'personal',
        ownerId: requesterKey('telegram-main', 'xu-ning'),
        title: '周末喜欢慢一点',
        body: '许宁偏好轻松的步行路线、独立书店和安静的咖啡店。一天不安排超过三个地点。',
        source: '许宁 · Telegram',
        updatedAt: now - 600_000,
      },
      {
        id: 'qq-preference',
        scope: 'personal',
        ownerId: requesterKey('qq-main', 'zhou-ran'),
        title: '会议笔记保留行动项',
        body: '周然希望会议汇总包含明确的行动项、负责人和待确认的问题。',
        source: '周然 · QQ',
        updatedAt: now - 500_000,
      },
    ],
    notifications: [
      {
        id: 'seed-approval',
        eventKey: 'qq-files:1:approval',
        conversationId: qq,
        taskId: files.id,
        kind: 'approval',
        read: false,
        title: files.title,
        detail: '请在 QQ 中确认文件操作',
      },
      {
        id: 'seed-complete',
        eventKey: 'weekend-plan:1:completed',
        conversationId: telegram,
        taskId: travel.id,
        kind: 'completed',
        read: false,
        title: travel.title,
        detail: '小队已完成，结果已回到 Telegram 会话',
      },
    ],
  };
}
