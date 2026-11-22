import { test, expect } from '@playwright/test';

test('content-first home, read-only channel previews and notification routing', async ({
  page,
}) => {
  const errors: string[] = [];
  page.on('pageerror', (error) => errors.push(error.message));
  await page.goto('/');
  await expect(page.getByRole('heading', { name: '今天想完成什么？' })).toBeVisible();
  await page.screenshot({
    path: 'test-results/home-light.png',
    fullPage: true,
    animations: 'disabled',
  });
  await page.getByTestId('conversation-qq-main').click();
  await expect(page.getByTestId('composer')).toHaveCount(0);
  await expect(page.getByRole('button', { name: '允许这次操作' })).toHaveCount(0);
  await expect(page.getByText('渠道会话只读')).toBeVisible();
  await page.getByTestId('task-card-qq-files').click();
  const panel = page.getByTestId('task-panel');
  await expect(panel.getByRole('button', { name: '停止任务' })).toHaveCount(0);
  await expect(panel.getByRole('button', { name: '重新尝试' })).toHaveCount(0);
  await expect(panel.getByText('渠道任务仅供预览')).toBeVisible();
  await panel.getByRole('button', { name: '查看所属对话' }).click();
  await expect(panel).toHaveCount(0);
  await expect(page.getByTestId('composer')).toHaveCount(0);
  await expect(page.getByTestId('task-card-qq-files')).toBeVisible();
  await page.getByRole('button', { name: '通知', exact: true }).click();
  const dialog = page.getByRole('dialog');
  await dialog.getByRole('button', { name: /找一个周末可以散步的地方/ }).click();
  await expect(panel.getByRole('heading', { name: '找一个周末可以散步的地方' })).toBeVisible();
  await expect(page.getByTestId('composer')).toHaveCount(0);
  await expect(page.locator('.artifact-paper [data-rich-text]')).toBeVisible();
  await page.screenshot({
    path: 'test-results/channel-workbench.png',
    fullPage: true,
    animations: 'disabled',
  });
  expect(errors).toEqual([]);
});

test('local approval, denial, retry and artifacts work through the interface', async ({ page }) => {
  await page.clock.install();
  await page.goto('/');
  await page.getByRole('textbox', { name: '给助手发消息' }).fill('帮我整理文件');
  await page.getByRole('button', { name: '发送消息' }).click();
  await page.clock.runFor(2400);
  await expect(page.getByRole('button', { name: '允许这次操作' })).toBeVisible();
  await expect(page.locator('.thinking-dots')).toHaveCount(0);
  await page.getByRole('button', { name: '拒绝', exact: true }).click();
  await expect(page.getByText('已拒绝此操作')).toBeVisible();
  await page.locator('.thread .task-card').click();
  await page.getByRole('button', { name: '重新尝试', exact: true }).click();
  await page.clock.runFor(2400);
  await page.getByTestId('task-panel').getByRole('button', { name: '允许这次操作' }).click();
  await page.clock.runFor(10_000);
  await page.getByTestId('task-panel').getByRole('button', { name: '成果', exact: true }).click();
  await expect(page.locator('.artifact-paper')).toContainText('帮我整理文件');
  await page.getByRole('button', { name: '原文', exact: true }).click();
  await expect(page.locator('.artifact-paper pre code')).toContainText('# 帮我整理文件');
  await expect(page.locator('.artifact-paper [data-rich-text]')).toHaveCount(0);
  await expect(page.getByRole('button', { name: '变更', exact: true })).toHaveCount(0);
  await page.getByRole('button', { name: '预览', exact: true }).click();
  await expect(page.locator('.artifact-paper [data-rich-text]')).toBeVisible();
});

test('close/reload interrupts runs, drafts survive appearance changes, both languages work', async ({
  page,
}) => {
  await page.clock.install();
  await page.goto('/');
  await page.getByRole('textbox', { name: '给助手发消息' }).fill('组织小队做一个计划');
  await page.getByRole('button', { name: '发送消息' }).click();
  await page.clock.runFor(800);
  await page.reload();
  await expect(page.locator('.thread .status')).toHaveText('已中断');
  await page.locator('.thread .task-card').first().click();
  const interrupted = page.getByTestId('task-panel');
  await interrupted.getByRole('button', { name: '小队', exact: true }).click();
  await expect(
    interrupted.locator('.member-state').filter({ hasText: '已停止' }).first(),
  ).toBeVisible();
  await interrupted.getByRole('button', { name: '关闭', exact: true }).click();
  await page.getByRole('textbox', { name: '给助手发消息' }).fill('还没发送的想法');
  await page.getByTestId('nav-settings').click();
  await page.getByTestId('theme-dark').click();
  await expect(page.locator('html')).toHaveAttribute('data-theme', 'dark');
  await page.getByTestId('language-select').selectOption('en');
  await expect(page.getByRole('heading', { name: 'Settings', exact: true })).toBeVisible();
  await page.screenshot({
    path: 'test-results/settings-dark-en.png',
    fullPage: true,
    animations: 'disabled',
  });
  await page.getByTestId('nav-assistant').click();
  await expect(page.getByRole('textbox', { name: 'Message your assistant' })).toHaveValue(
    '还没发送的想法',
  );
  await page.emulateMedia({ colorScheme: 'dark', reducedMotion: 'reduce' });
  await page.getByTestId('nav-settings').click();
  await page.getByTestId('theme-system').click();
  await expect(page.locator('html')).toHaveAttribute('data-theme', 'dark');
  await page.emulateMedia({ colorScheme: 'light' });
  await expect(page.locator('html')).toHaveAttribute('data-theme', 'light');
});

test('memory edits, deletion, search, connection failure and retry are functional', async ({
  page,
}) => {
  await page.clock.install();
  await page.goto('/');
  await page.getByTestId('nav-memory').click();
  await page.getByRole('button', { name: /让资料各归其位/ }).click();
  await page.getByRole('textbox', { name: '标题', exact: true }).fill('清楚的工作区约定');
  await page.getByRole('button', { name: '保存记忆' }).click();
  await expect(page.getByRole('button', { name: /清楚的工作区约定/ })).toBeVisible();
  await page.getByRole('textbox', { name: '搜索记忆' }).fill('清楚的工作区');
  await expect(page.locator('.memory-card')).toHaveCount(1);
  await page.locator('.memory-card').click();
  await page.getByRole('button', { name: '删除', exact: true }).click();
  await page.getByRole('button', { name: '确认删除', exact: true }).click();
  await expect(page.locator('.memory-card')).toHaveCount(0);
  await page.getByTestId('nav-connections').click();
  const connection = page
    .locator('.connection-card')
    .filter({ has: page.getByRole('heading', { name: '飞书', exact: true }) });
  await connection.getByRole('button', { name: '配置', exact: true }).click();
  await page.getByRole('combobox', { name: '演示连接结果' }).selectOption('error');
  await page.getByRole('button', { name: '保存并模拟连接' }).click();
  await page.clock.runFor(800);
  await expect(connection.getByText('连接异常')).toBeVisible();
  await connection.getByRole('button', { name: '配置', exact: true }).click();
  await page.getByRole('textbox', { name: '示例账号名称' }).fill('Orangutan · 示例账号');
  await page.getByRole('combobox', { name: '演示连接结果' }).selectOption('success');
  await page.getByRole('button', { name: '保存并模拟连接' }).click();
  await page.clock.runFor(800);
  await expect(connection.getByText('已连接', { exact: true })).toBeVisible();
  await connection.getByRole('button', { name: '断开', exact: true }).click();
  await expect(connection.getByText('未连接', { exact: true })).toBeVisible();
});

test('narrow layout, search shortcut and dialog focus remain usable', async ({ page }) => {
  await page.setViewportSize({ width: 700, height: 850 });
  await page.goto('/');
  await page.getByRole('button', { name: '打开导航' }).click();
  await page.getByTestId('nav-tasks').click();
  await expect(page.getByRole('heading', { name: '任务', exact: true })).toBeVisible();
  await page.keyboard.press('Control+k');
  const search = page.getByRole('textbox', { name: '搜索关键词' });
  await expect(search).toBeFocused();
  await search.fill('周末');
  await page
    .getByRole('dialog')
    .getByRole('button', { name: /找一个周末可以散步的地方/ })
    .click();
  await expect(page.getByTestId('task-panel')).toBeVisible();
  expect(await page.evaluate(() => document.documentElement.scrollWidth <= innerWidth)).toBe(true);
  await expect(page.locator('.artifact-paper [data-rich-text]')).toBeVisible();
  await page.screenshot({
    path: 'test-results/narrow-workbench.png',
    fullPage: true,
    animations: 'disabled',
  });
  await page.getByTestId('task-panel').getByRole('button', { name: '关闭', exact: true }).click();
  await page.getByRole('button', { name: '通知', exact: true }).click();
  await page.keyboard.press('Escape');
  await expect(page.getByRole('button', { name: '通知', exact: true })).toBeFocused();
});

test('corrupt storage is preserved and dangerous markdown is not executed', async ({ page }) => {
  await page.addInitScript(() => localStorage.setItem('orangutan.desktop.demo', '{broken'));
  await page.goto('/');
  await expect(page.getByRole('alert')).toBeVisible();
  expect(await page.evaluate(() => localStorage.getItem('orangutan.desktop.demo'))).toBe('{broken');
  await page
    .getByRole('textbox', { name: '给助手发消息' })
    .fill('正常内容\n\n<script>window.__unsafe = true</script>\n\n[unsafe](javascript:alert(1))');
  await page.getByRole('button', { name: '发送消息' }).click();
  await expect(page.locator('.message.user [data-rich-text]')).toBeVisible();
  expect(await page.evaluate(() => '__unsafe' in window)).toBe(false);
  expect(await page.locator('a[href^="javascript:"]').count()).toBe(0);
});

test('page lifecycle stops demo effects and a cached page resumes interrupted', async ({
  page,
}) => {
  await page.clock.install();
  await page.goto('/');
  await page.getByRole('textbox', { name: '给助手发消息' }).fill('组织小队做一个计划');
  await page.getByRole('button', { name: '发送消息' }).click();
  await page.clock.runFor(800);
  const snapshot = () =>
    page.evaluate(() => JSON.parse(localStorage.getItem('orangutan.desktop.demo')!).tasks[0]);
  const before = await page.evaluate(() => {
    window.dispatchEvent(new PageTransitionEvent('pagehide', { persisted: true }));
    return JSON.parse(localStorage.getItem('orangutan.desktop.demo')!).tasks[0];
  });
  await page.clock.runFor(6000);
  expect((await snapshot()).step).toBe(before.step);
  await page.evaluate(() =>
    window.dispatchEvent(new PageTransitionEvent('pageshow', { persisted: true })),
  );
  await expect(page.locator('.thread .status')).toHaveText('已中断');
  await page.clock.runFor(1500);
  expect((await snapshot()).status).toBe('interrupted');
});

test('execution summaries stay beside the request that created them', async ({ page }) => {
  await page.clock.install();
  await page.goto('/');
  const input = page.getByRole('textbox', { name: '给助手发消息' });
  await input.fill('第一项：讨论产品目标');
  await page.getByRole('button', { name: '发送消息' }).click();
  await expect(page.locator('.thread > .message.user + .thread-task')).toHaveCount(1);
  await expect(page.locator('.thread-date')).toHaveText(['今天']);
  const firstTaskId = await page.locator('.thread-task .task-card').getAttribute('data-testid');
  await page.clock.runFor(12_000);
  await expect(page.locator('.thread-task .status')).toHaveText('已完成');
  await input.fill('第二项：讨论下一步');
  await page.getByRole('button', { name: '发送消息' }).click();
  await expect(page.locator('.thread > .message.user + .thread-task')).toHaveCount(2);
  await expect(page.locator('.thread-task .task-card').first()).toHaveAttribute(
    'data-testid',
    firstTaskId!,
  );
  await expect(page.locator('.thread > .thread-task + .message.assistant')).toHaveCount(1);
  await expect(page.locator('.thread > .message.user').first()).toContainText('第一项');
  await expect(page.locator('.thread > .message.user').last()).toContainText('第二项');
});

test('navigation drawer contains focus and hands off to search', async ({ page }) => {
  await page.setViewportSize({ width: 700, height: 850 });
  await page.goto('/');
  const menu = page.getByRole('button', { name: '打开导航', exact: true });
  const first = page.locator('.sidebar button').first();
  const last = page.locator('.sidebar button').last();
  await menu.click();
  await expect(first).toBeFocused();
  await expect(page.locator('.workspace')).toHaveAttribute('inert', '');
  await page.keyboard.press('Shift+Tab');
  await expect(last).toBeFocused();
  await page.keyboard.press('Tab');
  await expect(first).toBeFocused();
  await page.keyboard.press('Escape');
  await expect(menu).toBeFocused();
  await expect(menu).toHaveAttribute('aria-expanded', 'false');
  await expect(page.locator('.workspace')).not.toHaveAttribute('inert', '');
  await menu.click();
  await page.keyboard.press('Control+k');
  await expect(menu).toHaveAttribute('aria-expanded', 'false');
  await expect(page.getByRole('dialog')).toBeVisible();
  await expect(page.getByRole('textbox', { name: '搜索关键词' })).toBeFocused();
});

test('reading earlier messages pauses following until Jump to latest is used', async ({ page }) => {
  await page.clock.install();
  await page.goto('/');
  const longRequest = Array.from(
    { length: 50 },
    (_, index) => `第 ${index + 1} 点：讨论产品目标与使用场景。`,
  ).join('\n\n');
  await page.getByRole('textbox', { name: '给助手发消息' }).fill(longRequest);
  await page.getByRole('button', { name: '发送消息' }).click();
  await page.clock.runFor(800);
  const scroll = page.locator('.conversation-scroll');
  await expect
    .poll(() => scroll.evaluate((element) => element.scrollHeight > element.clientHeight + 200))
    .toBe(true);
  await scroll.evaluate((element) => {
    element.scrollTop = 0;
  });
  await expect(page.getByRole('button', { name: '回到最新' })).toBeVisible();
  await page.clock.runFor(4000);
  expect(await scroll.evaluate((element) => element.scrollTop)).toBeLessThan(2);
  await page.getByRole('button', { name: '回到最新' }).click();
  await expect(page.getByRole('button', { name: '回到最新' })).toHaveCount(0);
  await expect(scroll).toBeFocused();
  await page.clock.runFor(2000);
  await expect
    .poll(() =>
      scroll.evaluate((element) => element.scrollHeight - element.scrollTop - element.clientHeight),
    )
    .toBeLessThan(64);
});

test('rename preserves a custom title after sending and reload, with cancel and channel restrictions', async ({
  page,
}) => {
  await page.goto('/');
  const rename = page.getByRole('button', { name: '重命名对话', exact: true });
  await rename.click();
  const input = page.getByRole('textbox', { name: '对话名称' });
  await expect(input).toBeFocused();
  await input.fill('   ');
  await expect(page.getByRole('button', { name: '保存名称' })).toBeDisabled();
  await input.fill('不应保存');
  await page.keyboard.press('Escape');
  await expect(rename).toBeFocused();
  await expect(page.locator('.conversation-heading')).not.toContainText('不应保存');
  await rename.click();
  await input.fill('产品设计讨论');
  await input.press('Enter');
  await expect(page.locator('.conversation-heading')).toContainText('产品设计讨论');
  await page.getByRole('textbox', { name: '给助手发消息' }).fill('讨论界面');
  await page.getByRole('button', { name: '发送消息' }).click();
  await page.reload();
  await expect(page.locator('.conversation-heading')).toContainText('产品设计讨论');
  await expect(
    page.locator('.sidebar').getByRole('button', { name: '产品设计讨论', exact: true }),
  ).toBeVisible();
  await page.getByTestId('conversation-qq-main').click();
  await expect(rename).toHaveCount(0);
});

test('search palette names sources and opens results from the keyboard', async ({ page }) => {
  await page.goto('/');
  await page.keyboard.press('Control+k');
  const dialog = page.getByRole('dialog');
  const search = dialog.getByRole('textbox', { name: '搜索关键词' });
  await search.fill('QQ · 工作');
  const result = dialog.getByRole('button', { name: /周然 · 工作/ });
  await expect(result).toContainText('QQ · 工作');
  await expect(dialog).not.toContainText('qq-studio');
  await search.press('ArrowDown');
  await expect(result).toBeFocused();
  await page.keyboard.press('ArrowUp');
  await expect(search).toBeFocused();
  await search.press('Enter');
  await expect(dialog).toHaveCount(0);
  await expect(page.locator('.conversation-heading')).toContainText('周然 · 工作');
  await expect(page.getByText('渠道会话只读')).toBeVisible();
});

test('secondary text keeps AA contrast on every surface in both themes', async ({ page }) => {
  await page.goto('/');
  for (const theme of ['light', 'dark']) {
    const ratios = await page.evaluate((theme) => {
      document.documentElement.dataset.theme = theme;
      const style = getComputedStyle(document.documentElement);
      const luminance = (name: string) => {
        const hex = style.getPropertyValue(name).trim().slice(1);
        const [r, g, b] = [0, 2, 4].map((i) => {
          const c = parseInt(hex.slice(i, i + 2), 16) / 255;
          return c <= 0.03928 ? c / 12.92 : ((c + 0.055) / 1.055) ** 2.4;
        });
        return 0.2126 * r + 0.7152 * g + 0.0722 * b;
      };
      return ['--muted', '--subtext'].flatMap((text) =>
        ['--bg', '--sidebar', '--surface', '--panel', '--hover'].map((surface) => {
          const [light, dark] = [luminance(text), luminance(surface)].sort((a, b) => b - a);
          return { pair: `${theme} ${text} on ${surface}`, ratio: (light + 0.05) / (dark + 0.05) };
        }),
      );
    }, theme);
    for (const { pair, ratio } of ratios) expect(ratio, pair).toBeGreaterThanOrEqual(4.5);
  }
});
