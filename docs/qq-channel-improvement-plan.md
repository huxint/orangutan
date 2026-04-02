# QQ Channel 改进计划

> 参考实现: [openclaw-qqbot](https://github.com/tencent-connect/openclaw-qqbot) (TypeScript)
> 当前实现: `src/channel/qq/` (C++)
> 编写日期: 2026-04-02

---

## 一、当前实现状态总结

### 已实现的功能

| 功能 | 状态 | 文件 |
|------|------|------|
| WebSocket 连接 (libcurl) | ✅ 完成 | `qq-transport.cpp` |
| Gateway 协议 (op 0/1/2/6/7/9/10/11) | ✅ 完成 | `qq-channel.cpp` |
| Token 获取与缓存 (5分钟提前刷新) | ✅ 完成 | `qq-channel.cpp:200-224` |
| 心跳机制 (独立线程) | ✅ 完成 | `qq-channel.cpp:336-375` |
| Session IDENTIFY / RESUME | ✅ 完成 | `qq-channel.cpp:287-323` |
| C2C 私聊收发 | ✅ 完成 | `qq-channel.cpp:475-524` |
| 群聊收发 | ✅ 完成 | `qq-channel.cpp:493-539` |
| 文本消息分片 (4000字符, 换行/空格优先) | ✅ 完成 | `qq-channel.cpp:550-577` |
| 指数退避重连 (1s/2s/4s/8s/15s) | ✅ 完成 | `reconnect-backoff.hpp` |
| 多 Bot 实例支持 (bot_name) | ✅ 完成 | `qq-channel.hpp` |
| JID 路由 (`qqbot:<name>:c2c:<id>` / `qqbot:<name>:group:<id>`) | ✅ 完成 | `qq-channel.cpp` |
| 条件编译 (`ORANGUTAN_ENABLE_QQ_CHANNEL`) | ✅ 完成 | 各文件 |

---

## 二、缺失功能清单 (按优先级排序)

### P0 — 核心缺失 (影响基本使用)

#### 1. 消息 ID 回传 (msg_id / msg_seq)

**现状**: 发送消息时没有携带 `msg_id`，QQ API 要求被动回复必须携带原消息 ID，否则会被视为主动消息受频率限制。

**需要做的事**:
- `InboundMessage` 增加字段 `message_id` (原始消息 ID)
- `handle_c2c_message()` / `handle_group_message()` 提取 `data["id"]` 并存入 `message_id`
- `send_c2c()` / `send_group()` 发送时携带 `msg_id` 字段
- 实现 `msg_seq` 自增计数器 (0-65535 循环)，每次发送递增

**参考数据结构**:
```cpp
// InboundMessage 增加
std::string message_id;  // QQ 原始消息 ID, 用于被动回复

// 发送时的 payload
{
    "content": "<text>",
    "msg_type": 0,
    "msg_id": "<inbound_message_id>",  // 被动回复
    "msg_seq": 1                        // 自增序号
}
```

**涉及文件**:
- `src/channel/channel.hpp` — `InboundMessage` 增加 `message_id`
- `src/channel/qq/qq-channel.hpp` — 增加 `msg_seq_` 原子计数器
- `src/channel/qq/qq-channel.cpp` — `handle_*_message()` 提取 ID, `send_*()` 携带 ID

---

#### 2. 主动消息 vs 被动回复区分

**现状**: 所有发送都走同一路径，无法区分是回复用户消息还是主动推送。

**需要做的事**:
- `send_message()` 接口增加可选参数 `reply_to_message_id`
- 被动回复: 在 payload 中携带 `msg_id`（1小时内有效，最多回复4次）
- 主动消息: 不带 `msg_id`，受限于频率（需要用户先与 bot 交互过）
- 维护 message reply tracker: 记录每条消息的回复次数和时间

**数据结构**:
```cpp
struct MessageReplyTracker {
    std::string message_id;
    std::chrono::steady_clock::time_point received_at;
    int reply_count = 0;
    static constexpr int max_replies = 4;
    static constexpr auto ttl = std::chrono::hours(1);
    
    bool can_reply() const {
        return reply_count < max_replies 
            && (std::chrono::steady_clock::now() - received_at) < ttl;
    }
};
```

---

#### 3. API 调用重试与错误处理

**现状**: `http_post()` 调用没有重试逻辑，401/429/5xx 错误直接失败。

**需要做的事**:
- 封装 `qq_api_request()` 统一处理:
  - **401**: Token 过期 → 清除缓存 → 重新获取 → 重试一次
  - **429**: 速率限制 → 读取 `Retry-After` header → 等待后重试
  - **502/503/504**: 网关错误 → 指数退避重试 (最多2次)
  - **其他 4xx**: 直接失败，记录错误日志
- 提取 QQ 业务错误码 (`code` 字段) 进行精细化处理
- 记录 `x-tps-trace-id` 响应头用于调试

**实现建议**:
```cpp
struct QqApiResponse {
    int http_status;
    std::string body;
    std::string trace_id;
    int biz_code = 0;        // QQ 业务错误码
    std::string biz_message;
};

QqApiResponse qq_api_post(const std::string &path, const nlohmann::json &body);
// 内部处理 token 刷新、重试、错误码解析
```

---

#### 4. 自动重连机制

**现状**: WebSocket 断连后 `Transport` 内部有 `request_reconnect()`，但 `QqChannel` 层面没有完整的重连循环。断连后 `connected_` 设为 false 就结束了。

**需要做的事**:
- 在 `on_close` / `on_error` 回调中触发自动重连流程
- 重连前尝试 RESUME (如果 session_id 有效)
- RESUME 失败 (op=9) 则清除 session 重新 IDENTIFY
- 使用已有的 `ReconnectBackoff` 控制重试间隔
- 成功连接后 reset backoff
- 可配置最大重连次数或无限重连

**伪代码**:
```
on_disconnect:
    if close_requested: return  // 用户主动断开
    while not close_requested:
        delay = backoff.next_delay()
        sleep(delay)
        try:
            ensure_access_token()
            gateway_url = get_gateway_url()
            connect_websocket(gateway_url)
            wait_for_ready()
            backoff.reset()
            return
        catch:
            log_warn("Reconnect failed, retrying...")
```

---

### P1 — 重要功能 (提升体验)

#### 5. 富媒体消息支持

**现状**: 只支持 `msg_type=0` (纯文本)。

**需要实现的消息类型**:

| msg_type | 类型 | 说明 |
|----------|------|------|
| 0 | 文本 | ✅ 已实现 |
| 1 | 图文混排 | 需要实现 |
| 2 | Markdown | 需要实现 |
| 3 | Ark 模板 | 需要实现 |
| 4 | Embed | 需要实现 |
| 7 | 富媒体 (图片/语音/视频/文件) | **优先实现** |

**重点: msg_type=7 富媒体发送流程**:

```
Step 1: 上传媒体获取 file_info
POST /v2/users/{openid}/files  (C2C)
POST /v2/groups/{openid}/files (群聊)
Body: {
    "file_type": 1,          // 1=图片 2=视频 3=语音 4=文件
    "url": "https://...",    // 网络URL
    "srv_send_msg": false    // false=仅上传不发送
}
Response: { "file_info": "..." }

Step 2: 用 file_info 发送消息
POST /v2/users/{openid}/messages
Body: {
    "msg_type": 7,
    "media": { "file_info": "<从step1获取>" },
    "msg_id": "<reply_to>"
}
```

**需要的接口**:
```cpp
// 上传媒体
std::string upload_media_c2c(const std::string &openid, int file_type, const std::string &url);
std::string upload_media_group(const std::string &openid, int file_type, const std::string &url);

// 发送媒体
void send_media_c2c(const std::string &openid, const std::string &file_info, const std::string &msg_id = "");
void send_media_group(const std::string &openid, const std::string &file_info, const std::string &msg_id = "");
```

**接收附件处理**:
- `InboundMessage` 增加 `attachments` 字段
- 解析 `data["attachments"]` 数组
- 提取 `content_type`, `url`, `filename`

```cpp
struct Attachment {
    std::string content_type;  // "image/png", "voice", "video/mp4", ...
    std::string url;
    std::string filename;
    int width = 0;
    int height = 0;
    int size = 0;
};

// InboundMessage 增加
std::vector<Attachment> attachments;
```

---

#### 6. Markdown 消息支持

**现状**: 不支持发送 Markdown 格式消息。

**需要做的事**:
```cpp
// msg_type=2 Markdown 消息
{
    "msg_type": 2,
    "markdown": {
        "content": "# 标题\n**粗体** *斜体*\n- 列表项"
    },
    "msg_id": "<reply_to>"
}
```

**QQ 支持的 Markdown 子集**:
- 标题 (`#`, `##`, `###`)
- 粗体 (`**text**`)
- 斜体 (`*text*`)
- 代码块 (`` `code` ``, ````\ncode\n````)
- 列表 (`-`, `1.`)
- 链接 (`[text](url)`)

---

#### 7. 按钮/键盘消息 (Keyboard)

**现状**: 不支持。

**需要做的事**:
- 发送带按钮的消息 (keyboard payload)
- 处理 `INTERACTION_CREATE` 事件回调

```cpp
// 发送带键盘的消息
{
    "msg_type": 2,
    "markdown": { "content": "请选择操作:" },
    "keyboard": {
        "content": {
            "rows": [{
                "buttons": [{
                    "id": "btn_1",
                    "render_data": { "label": "选项A", "style": 1 },
                    "action": {
                        "type": 2,           // 回调
                        "data": "action_a",
                        "permission": { "type": 2 }  // 所有人可点
                    }
                }]
            }]
        }
    }
}
```

**处理交互回调**:
```cpp
// handle_dispatch 增加
if (event_type == "INTERACTION_CREATE") {
    handle_interaction(data);
}

void handle_interaction(const nlohmann::json &data) {
    auto button_data = data["data"]["resolved"]["button_data"];
    auto user_openid = data["user_openid"];
    // 回调确认 (必须在5秒内响应)
    // PUT /interactions/{interaction_id}
}
```

---

#### 8. 接收消息的附件/图片下载

**现状**: 收到带附件的消息时只提取了 `content` 文本，忽略了 `attachments`。

**需要做的事**:
- 解析 `data["attachments"]` 数组
- 对图片/文件 URL 进行下载 (需要 Authorization header)
- 将附件信息传递给上层处理

---

### P2 — 增强功能 (完善度)

#### 9. Mention (@ 提及) 处理

**现状**: 不处理 @ 信息。群消息中的 `@bot` 内容会原样传给上层。

**需要做的事**:
- 解析 `data["mentions"]` 数组，识别 bot 自身是否被 @
- 从消息内容中剥离 `<@member_openid>` 标签
- 在 `InboundMessage` 中标记 `bool mentioned = false`
- 可选: 支持 `requireMention` 配置 (群聊中必须 @ 才响应)

```cpp
// InboundMessage 增加
bool mentioned = false;          // bot 是否被 @
std::vector<std::string> mention_ids;  // 所有被 @ 的人

// 剥离 @ 标签
std::string strip_mentions(const std::string &content);
// "<@!abc123> 你好" -> "你好"
```

---

#### 10. 频道/Guild 消息支持

**现状**: Intent 中订阅了 `intent_guild_at_message`，但 `handle_dispatch()` 中没有处理 guild 相关事件。

**需要做的事**:
- 处理 `GUILD_MESSAGE_CREATE` / `AT_MESSAGE_CREATE` 事件
- 增加 JID 格式: `qqbot:<name>:guild:<channel_id>`
- 实现 `send_guild()`: `POST /channels/{channel_id}/messages`
- Guild 消息格式与 C2C/Group 略有不同 (有 `channel_id`, `guild_id`)

---

#### 11. 消息引用/回复

**现状**: 不支持引用回复。

**需要做的事**:
- 发送时支持 `message_reference` 字段
- 接收时解析 `message_scene.ext` 中的 `ref_msg_idx`

```cpp
// 发送引用回复
{
    "content": "回复内容",
    "msg_type": 0,
    "msg_id": "<original_msg_id>",
    "message_reference": {
        "message_id": "<被引用的消息ID>",
        "ignore_get_message_error": true
    }
}
```

---

#### 12. 表情回应 (Reaction)

**现状**: 不支持。

**需要做的事**:
- 订阅 `MESSAGE_REACTION_ADD` / `MESSAGE_REACTION_REMOVE` 意图
- 提供发送 reaction 的 API

```
PUT /channels/{channel_id}/messages/{message_id}/reactions/{type}/{id}
DELETE /channels/{channel_id}/messages/{message_id}/reactions/{type}/{id}
```

---

#### 13. Token 后台刷新

**现状**: Token 仅在 API 调用时按需刷新 (`ensure_access_token()`)，没有后台定时刷新。

**需要做的事**:
- 启动一个后台线程，在 token 过期前 1/3 TTL 时主动刷新
- 避免在高并发 API 调用时多个线程同时刷新 token
- 断连时停止后台刷新

```cpp
void start_token_refresh_loop();
void stop_token_refresh_loop();
// token 默认 7200s，在 ~2400s 时刷新
```

---

#### 14. Session 持久化

**现状**: Session (session_id, last_seq) 仅存在于内存中，进程重启后丢失。

**需要做的事**:
- 将 session 状态写入磁盘 (JSON 文件)
- 启动时读取并尝试 RESUME
- 5分钟 TTL (QQ 服务端限制)
- 按 bot_name 隔离存储

```
~/.orangutan/qq/sessions/session-{bot_name}.json
{
    "session_id": "...",
    "last_seq": 42,
    "app_id": "...",
    "saved_at": "2026-04-02T12:00:00Z"
}
```

---

### P3 — 进阶功能 (参考实现中的高级特性)

#### 15. 消息发送去抖动 (Debounce)

**说明**: 当 AI 分多段输出时，将短时间内的多段文本合并为一条消息发送，减少消息条数。

**实现方案**:
```cpp
class MessageDebouncer {
    std::chrono::milliseconds window_{1500};   // 合并窗口
    std::chrono::milliseconds max_wait_{8000}; // 最大等待
    std::string separator_ = "\n\n---\n\n";
    
    // 按 jid 维护 pending buffer
    std::unordered_map<std::string, PendingMessage> pending_;
    
    void enqueue(const std::string &jid, const std::string &text);
    void flush(const std::string &jid);  // 合并发送
};
```

---

#### 16. 群消息历史聚合

**说明**: 群聊中多人连续发言时，将这些消息合并为一条上下文传给 AI，而不是每条都触发一次 AI 回复。

**实现方案**:
- 维护 per-group 消息缓冲区 (最多50条)
- 当 bot 被 @ 时，将缓冲区内容作为上下文一并传给 AI
- 未被 @ 时仅记录不触发回复

---

#### 17. 已知用户列表 (Proactive Message)

**说明**: 记录所有与 bot 交互过的用户，用于后续主动推送。

**需要做的事**:
- 每次收到消息时记录 openid + 类型 (c2c/group)
- 持久化到磁盘
- 提供查询接口

---

#### 18. Typing Indicator (输入状态)

**说明**: 在 AI 处理期间显示 "正在输入..." 状态。

**需要做的事**: 暂无公开 API，参考实现使用的是平台特定接口，可暂缓。

---

## 三、架构改进建议

### 1. 统一 API 层

当前 HTTP 调用直接使用 `http_post()`/`http_get()`，建议抽取统一的 QQ API 客户端:

```cpp
class QqApiClient {
public:
    QqApiClient(std::string app_id, std::string client_secret);
    
    // 统一的请求方法，处理 token/重试/错误码
    nlohmann::json get(const std::string &path);
    nlohmann::json post(const std::string &path, const nlohmann::json &body);
    
    // 业务方法
    std::string get_gateway_url();
    void send_c2c_message(const std::string &openid, const nlohmann::json &payload);
    void send_group_message(const std::string &openid, const nlohmann::json &payload);
    std::string upload_media(const std::string &openid, int file_type, const std::string &url, bool is_group);

private:
    void ensure_token();
    void handle_error(int status, const nlohmann::json &body);
    
    std::string app_id_;
    std::string client_secret_;
    std::string token_;
    std::chrono::steady_clock::time_point token_expiry_;
    std::mutex token_mutex_;
    std::atomic<uint16_t> msg_seq_{0};
};
```

### 2. 消息构建器

```cpp
class QqMessageBuilder {
public:
    QqMessageBuilder &text(const std::string &content);
    QqMessageBuilder &markdown(const std::string &content);
    QqMessageBuilder &media(const std::string &file_info);
    QqMessageBuilder &reply_to(const std::string &msg_id);
    QqMessageBuilder &keyboard(const nlohmann::json &keyboard);
    QqMessageBuilder &reference(const std::string &ref_msg_id);
    
    nlohmann::json build() const;
    
private:
    int msg_type_ = 0;
    nlohmann::json payload_;
};
```

### 3. 事件分发器增强

```cpp
void QqChannel::handle_dispatch(const std::string &event_type, const nlohmann::json &data) {
    // 现有
    if (event_type == "C2C_MESSAGE_CREATE") return handle_c2c_message(data);
    if (event_type == "GROUP_AT_MESSAGE_CREATE") return handle_group_message(data);
    if (event_type == "GROUP_MESSAGE_CREATE") return handle_group_message(data);
    
    // 新增
    if (event_type == "INTERACTION_CREATE") return handle_interaction(data);
    if (event_type == "AT_MESSAGE_CREATE") return handle_guild_message(data);
    if (event_type == "GUILD_MEMBER_ADD") return handle_member_event(data);
    if (event_type == "MESSAGE_REACTION_ADD") return handle_reaction(data);
}
```

---

## 四、实现顺序建议

```
Phase 1 (基础修复):
  1.1  消息 ID 回传 (msg_id / msg_seq)           — P0 #1
  1.2  API 重试与错误处理                          — P0 #3
  1.3  自动重连机制                                — P0 #4

Phase 2 (回复增强):
  2.1  主动消息 vs 被动回复                        — P0 #2
  2.2  Mention 处理与剥离                          — P2 #9
  2.3  接收附件解析                                — P1 #8

Phase 3 (富媒体):
  3.1  富媒体消息发送 (msg_type=7)                 — P1 #5
  3.2  Markdown 消息                               — P1 #6
  3.3  消息引用/回复                               — P2 #11

Phase 4 (交互):
  4.1  按钮/键盘消息                               — P1 #7
  4.2  频道消息支持                                — P2 #10
  4.3  表情回应                                    — P2 #12

Phase 5 (健壮性):
  5.1  Token 后台刷新                              — P2 #13
  5.2  Session 持久化                              — P2 #14
  5.3  消息去抖动                                  — P3 #15

Phase 6 (高级):
  6.1  群消息历史聚合                              — P3 #16
  6.2  已知用户列表                                — P3 #17
```

---

## 五、需要修改的文件清单

| 文件 | 改动类型 | 涉及功能 |
|------|----------|----------|
| `src/channel/channel.hpp` | 修改 | `InboundMessage` 增加 `message_id`, `attachments`, `mentioned` |
| `src/channel/qq/qq-channel.hpp` | 修改 | 增加 API 客户端、msg_seq、重连线程、debouncer |
| `src/channel/qq/qq-channel.cpp` | 修改 | 几乎所有功能改进 |
| `src/channel/qq/qq-api-client.hpp` | **新建** | 统一 API 客户端 |
| `src/channel/qq/qq-api-client.cpp` | **新建** | API 客户端实现 |
| `src/channel/qq/qq-message-builder.hpp` | **新建** | 消息构建器 |
| `src/channel/qq/qq-transport.hpp` | 可能修改 | 重连逻辑增强 |
| `src/channel/qq/qq-transport.cpp` | 可能修改 | 重连逻辑增强 |
| `src/channel/qq/reconnect-backoff.hpp` | 不变 | 已满足需求 |

---

## 六、注意事项

1. **QQ API 频率限制**: 被动回复 (带 msg_id) 每条消息最多回复 4 次，1 小时内有效；主动消息有更严格的频率限制
2. **Token 有效期**: 默认 7200 秒，建议提前 5 分钟刷新
3. **消息长度限制**: 文本消息最大 4000 字符 (已实现分片)，Markdown 最大 5000 字符
4. **富媒体限制**: 图片/语音/视频/文件需先上传获取 file_info，再发送；大文件 (>10MB) 需分片上传
5. **WebSocket Session**: 仅 5 分钟内可 RESUME，超过需重新 IDENTIFY
6. **构建耗时**: xmake build 约 70 秒，实现时应批量修改后再构建，避免频繁编译
