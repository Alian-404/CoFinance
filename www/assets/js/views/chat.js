import { html, mount, qs, qsa } from '../core/dom.js';
import { api } from '../core/api.js';
import { getToken } from '../core/session.js';
import { icon } from '../core/icons.js';
import { avatar, errorBlock, friendlyError, loadingBlock, spinner, toast } from '../core/ui.js';
import {
  displayName,
  formatTime,
  localDayValue,
  parseDate,
  uuid,
} from '../core/format.js';
import {
  latestMessageId,
  mergeMessages,
  nextReconnectDelay,
  overlapAfterId,
} from '../core/messages.js';

const quickQuestions = [
  '这个月我们花了多少？',
  '谁花得最多？',
  '哪一类支出占比最高？',
];

export function renderChat({ root, gid, group, currentUser }) {
  let messages = [];
  let olderCursor = null;
  let serverMax = 0;
  let lastSeen = 0;
  let lastMarkedRead = 0;
  let loading = true;
  let loadError = null;
  let loadingOlder = false;
  let socket = null;
  let active = true;
  let reconnectTimer = 0;
  let reconnectAttempt = 0;
  let fallbackRunning = false;
  let fallbackController = null;
  let messageController = null;
  let aiController = null;
  let firstReady = true;
  let connectionState = 'connecting';
  let connectionLabel = '正在连接实时通道…';
  let aiMessages = [{ role: 'assistant', content: '我会只根据这本账的聚合统计回答。可以问我本月花了多少、谁花得最多，或让我给一条省钱建议。' }];
  let aiLoading = false;

  mount(root, html`
    <section class="tab-panel">
      <div class="page-heading-row">
        <div>
          <p class="eyebrow">Live together</p>
          <h2 class="page-title">把账聊明白</h2>
          <p class="page-subtitle">消息先落 MySQL，再实时推送到在线成员。断线后会补拉重叠窗口并按 id 去重。</p>
        </div>
      </div>
      <div class="chat-layout">
        <section class="chat-panel">
          <header class="chat-head">
            <div class="chat-head-copy">
              <h3>${group.name}</h3>
              <p class="card-subtitle">实时消息与共同账本同步</p>
            </div>
            <span class="connection-status ${connectionState}" data-connection-status>
              <span class="connection-dot"></span><span data-connection-label>${connectionLabel}</span>
            </span>
          </header>
          <div class="message-scroll" data-message-scroll>${loadingBlock('正在加载消息…')}</div>
          <form class="chat-compose" data-message-form>
            <textarea name="content" rows="1" maxlength="2000" placeholder="写一条消息，Enter 发送，Shift+Enter 换行" aria-label="消息内容"></textarea>
            <button class="button button-primary send-button" type="submit" aria-label="发送消息">${icon('send')}</button>
          </form>
        </section>

        <aside class="chat-side">
          <section class="card card-pad ai-chat-card">
            <div class="card-heading">
              <div class="ai-heading">
                <span class="ai-orb">${icon('sparkles')}</span>
                <div>
                  <h2 class="card-title">AI 财务助手</h2>
                  <p class="card-subtitle">单次问答，不保存会话历史</p>
                </div>
              </div>
            </div>
            <div class="ai-chat-body" data-ai-body></div>
            <form class="ai-question-form" data-ai-form>
              <textarea name="question" rows="1" maxlength="500" placeholder="问问这本账…" aria-label="向 AI 提问"></textarea>
              <button class="icon-button" type="submit" aria-label="发送问题">${icon('send')}</button>
            </form>
            <div class="quick-questions">
              <button class="quick-question" type="button" data-ai-suggestion>生成省钱建议</button>
              ${quickQuestions.map((question) => html`<button class="quick-question" type="button" data-quick-question="${question}">${question}</button>`)}
            </div>
            <p class="ai-note">提示：AI 可能出错，重要金额仍以账本数据为准。每用户每分钟最多 5 次。</p>
          </section>

          <section class="card card-pad">
            <div class="card-heading"><div><h2 class="card-title">实时策略</h2><p class="card-subtitle">WebSocket 优先，长轮询兜底</p></div></div>
            <div class="notice">${icon('info')}<span>连接后先收到 ready 与 server_max_id。重连时回拉最近 20 条重叠窗口，避免自增 id 提交乱序造成静默漏消息。</span></div>
          </section>
        </aside>
      </div>
    </section>
  `);

  const scroll = qs('[data-message-scroll]', root);
  const messageForm = qs('[data-message-form]', root);
  const messageInput = messageForm.elements.content;
  const sendButton = qs('button[type="submit"]', messageForm);
  const aiBody = qs('[data-ai-body]', root);
  const aiForm = qs('[data-ai-form]', root);
  const aiInput = aiForm.elements.question;
  const aiSend = qs('button[type="submit"]', aiForm);

  renderAiMessages();
  loadInitial();

  messageForm.addEventListener('submit', (event) => {
    event.preventDefault();
    sendMessage();
  });
  messageInput.addEventListener('keydown', (event) => {
    if (event.key === 'Enter' && !event.shiftKey) {
      event.preventDefault();
      sendMessage();
    }
  });
  messageInput.addEventListener('input', () => autoGrow(messageInput, 150));
  scroll.addEventListener('scroll', updateScrollAffordance, { passive: true });

  aiForm.addEventListener('submit', (event) => {
    event.preventDefault();
    const question = aiInput.value.trim();
    if (question) {
      askAi(question);
      aiInput.value = '';
      autoGrow(aiInput, 110);
    }
  });
  aiInput.addEventListener('keydown', (event) => {
    if (event.key === 'Enter' && !event.shiftKey) {
      event.preventDefault();
      aiForm.requestSubmit();
    }
  });
  aiInput.addEventListener('input', () => autoGrow(aiInput, 110));
  qsa('[data-quick-question]', root).forEach((button) => button.addEventListener('click', () => {
    aiInput.value = button.dataset.quickQuestion;
    aiForm.requestSubmit();
  }));
  qs('[data-ai-suggestion]', root)?.addEventListener('click', loadAiSuggestion);

  async function loadInitial() {
    loading = true;
    loadError = null;
    renderMessages();
    messageController?.abort();
    messageController = new AbortController();
    try {
      const page = await api.messages(gid, { limit: 30 }, { signal: messageController.signal });
      if (!active) {
        return;
      }
      messages = mergeMessages([], (page.items || []).slice().reverse());
      olderCursor = page.next_cursor || null;
      serverMax = Number(page.server_max_id || 0);
      lastSeen = Math.max(latestMessageId(messages), serverMax);
      loading = false;
      renderMessages({ forceBottom: true });
      window.requestAnimationFrame(markReadIfNeeded);
      connectSocket();
    } catch (error) {
      if (!active) {
        return;
      }
      loading = false;
      loadError = error;
      renderMessages();
    }
  }

  function renderMessages({ forceBottom = false } = {}) {
    if (loading) {
      mount(scroll, loadingBlock('正在加载消息…'));
      return;
    }
    if (loadError) {
      mount(scroll, errorBlock(loadError));
      qs('[data-action="retry"]', scroll)?.addEventListener('click', loadInitial);
      return;
    }
    const nearBottom = isNearBottom();
    let lastDay = '';
    const blocks = [];
    if (olderCursor) {
      blocks.push(html`<div class="older-row"><button class="button button-sm button-ghost" type="button" data-load-older ${loadingOlder ? 'disabled' : ''}>${loadingOlder ? spinner('加载中') : '加载更早消息'}</button></div>`);
    }
    for (const message of messages) {
      const date = parseDate(message.created_at);
      const day = date ? localDayValue(date) : '';
      if (day !== lastDay) {
        lastDay = day;
        blocks.push(html`<div class="message-day">${date ? dayLabel(date) : ''}</div>`);
      }
      blocks.push(messageRow(message));
    }
    if (messages.length === 0) {
      blocks.push(html`<div class="empty-state"><span class="empty-icon">${icon('chat')}</span><h3>还没有消息</h3><p>发出第一条消息，组内在线成员会立即收到。</p></div>`);
    }
    mount(scroll, html`${blocks}`);
    qs('[data-load-older]', scroll)?.addEventListener('click', loadOlder);
    if (forceBottom || nearBottom) {
      requestAnimationFrame(() => {
        scroll.scrollTop = scroll.scrollHeight;
      });
    }
    updateScrollAffordance();
  }

  function messageRow(message) {
    const isSystem = Number(message.sender_id) === 0;
    if (isSystem) {
      return html`<div class="message-row system"><div class="system-bubble">${message.content}</div></div>`;
    }
    const self = Number(message.sender_id) === Number(currentUser.id);
    return html`
      <div class="message-row ${self ? 'self' : ''}" data-message-id="${message.id}">
        ${self ? '' : avatar({ nickname: message.sender_name, avatar_url: message.sender_avatar_url }, { size: 'xs' })}
        <div class="message-bubble-wrap">
          ${self ? '' : html`<span class="message-author">${message.sender_name}</span>`}
          <div class="message-bubble">${message.content}</div>
          <span class="message-time">${formatTime(message.created_at)}</span>
        </div>
      </div>
    `;
  }

  function dayLabel(date) {
    const now = new Date();
    const today = localDayValue(now);
    const yesterdayDate = new Date(now.getFullYear(), now.getMonth(), now.getDate() - 1);
    const day = localDayValue(date);
    if (day === today) {
      return '今天';
    }
    if (day === localDayValue(yesterdayDate)) {
      return '昨天';
    }
    return new Intl.DateTimeFormat('zh-CN', { month: 'long', day: 'numeric', weekday: 'short' }).format(date);
  }

  async function loadOlder() {
    if (!olderCursor || loadingOlder) {
      return;
    }
    loadingOlder = true;
    const previousHeight = scroll.scrollHeight;
    const previousTop = scroll.scrollTop;
    renderMessages();
    try {
      const page = await api.messages(gid, { cursor: olderCursor, limit: 30 }, { signal: messageController?.signal });
      if (!active) {
        return;
      }
      messages = mergeMessages(messages, page.items || []);
      olderCursor = page.next_cursor || null;
    } catch (error) {
      toast(friendlyError(error), 'error', 4800);
    } finally {
      loadingOlder = false;
      renderMessages();
      if (!loadError) {
        window.requestAnimationFrame(() => {
          scroll.scrollTop = scroll.scrollHeight - previousHeight + previousTop;
        });
      }
    }
  }

  async function sendMessage() {
    const content = messageInput.value.trim();
    if (!content || sendButton.disabled) {
      return;
    }
    if ([...content].length > 2000) {
      toast('消息不能超过 2000 个字符', 'warning');
      return;
    }
    sendButton.disabled = true;
    try {
      const created = await api.sendMessage(gid, content, uuid());
      if (!active) {
        return;
      }
      messages = mergeMessages(messages, [created]);
      lastSeen = Math.max(lastSeen, Number(created.id || 0));
      messageInput.value = '';
      autoGrow(messageInput, 150);
      renderMessages({ forceBottom: true });
      markReadIfNeeded();
    } catch (error) {
      toast(friendlyError(error), 'error', 4800);
    } finally {
      sendButton.disabled = false;
      messageInput.focus();
    }
  }

  function connectSocket() {
    if (!active) {
      return;
    }
    setConnection('connecting', '正在连接实时通道…');
    const protocol = window.location.protocol === 'https:' ? 'wss:' : 'ws:';
    const url = `${protocol}//${window.location.host}/api/ws?token=${encodeURIComponent(getToken())}&group_id=${encodeURIComponent(gid)}`;
    try {
      socket = new WebSocket(url);
    } catch {
      scheduleReconnect();
      startFallback();
      return;
    }
    socket.addEventListener('open', () => {
      if (!active) {
        socket?.close();
        return;
      }
      setConnection('connecting', '握手完成，等待 ready…');
    });
    socket.addEventListener('message', (event) => handleSocketMessage(event.data));
    socket.addEventListener('close', () => {
      socket = null;
      if (!active) {
        return;
      }
      setConnection('offline', '实时连接已断开');
      startFallback();
      scheduleReconnect();
    });
    socket.addEventListener('error', () => {
      socket?.close();
    });
  }

  async function handleSocketMessage(raw) {
    let event;
    try {
      event = JSON.parse(raw);
    } catch {
      return;
    }
    if (event.type === 'ready') {
      reconnectAttempt = 0;
      setConnection('online', 'WebSocket 已连接');
      stopFallback();
      const readyMax = Number(event.server_max_id || 0);
      if (firstReady) {
        firstReady = false;
        serverMax = Math.max(serverMax, readyMax);
        if (lastSeen === 0) {
          lastSeen = serverMax;
        }
      } else {
        await recoverAfterReconnect(readyMax);
      }
      return;
    }
    if (event.type === 'message' && event.data) {
      const previousMax = latestMessageId(messages);
      messages = mergeMessages(messages, [event.data]);
      lastSeen = Math.max(lastSeen, Number(event.data.id || 0));
      serverMax = Math.max(serverMax, Number(event.data.id || 0));
      if (latestMessageId(messages) !== previousMax) {
        renderMessages({ forceBottom: false });
        markReadIfNeeded();
      }
      return;
    }
    if (event.type === 'error') {
      toast(event.message || 'WebSocket 操作失败', 'error', 4200);
    }
  }

  function scheduleReconnect() {
    window.clearTimeout(reconnectTimer);
    if (!active) {
      return;
    }
    const delay = nextReconnectDelay(reconnectAttempt++);
    reconnectTimer = window.setTimeout(connectSocket, delay);
  }

  async function recoverAfterReconnect(readyMax) {
    const after = overlapAfterId(Math.max(readyMax, lastSeen), 20);
    try {
      const page = await api.messages(gid, { after_id: after, limit: 100 });
      if (!active) {
        return;
      }
      const before = latestMessageId(messages);
      messages = mergeMessages(messages, page.items || []);
      serverMax = Math.max(serverMax, Number(page.server_max_id || 0));
      lastSeen = Math.max(lastSeen, latestMessageId(messages));
      if (latestMessageId(messages) !== before) {
        renderMessages({ forceBottom: false });
        markReadIfNeeded();
      }
    } catch (error) {
      if (!error.aborted) {
        toast(friendlyError(error), 'error', 4200);
      }
    }
  }

  async function startFallback() {
    if (!active || fallbackRunning) {
      return;
    }
    fallbackRunning = true;
    setConnection('fallback', '长轮询兜底中');
    while (active && fallbackRunning) {
      fallbackController = new AbortController();
      try {
        const page = await api.pollMessages(gid, lastSeen, 25, { signal: fallbackController.signal });
        if (!active || !fallbackRunning) {
          break;
        }
        const before = latestMessageId(messages);
        messages = mergeMessages(messages, page.items || []);
        serverMax = Math.max(serverMax, Number(page.server_max_id || 0));
        lastSeen = Math.max(lastSeen, latestMessageId(messages));
        if (latestMessageId(messages) !== before) {
          renderMessages({ forceBottom: false });
          markReadIfNeeded();
        }
      } catch (error) {
        if (!active || error.aborted) {
          break;
        }
        setConnection('offline', '实时通道与长轮询均不可用');
        await wait(1800);
      }
    }
    fallbackRunning = false;
  }

  function stopFallback() {
    fallbackRunning = false;
    fallbackController?.abort();
    fallbackController = null;
  }

  function setConnection(state, label) {
    connectionState = state;
    connectionLabel = label;
    const element = qs('[data-connection-status]', root);
    if (element) {
      element.className = `connection-status ${state}`;
      qs('[data-connection-label]', element).textContent = label;
    }
  }

  function markReadIfNeeded() {
    if (document.visibilityState !== 'visible' || !isNearBottom()) {
      return;
    }
    const latest = latestMessageId(messages);
    if (!latest || latest <= lastMarkedRead) {
      return;
    }
    lastMarkedRead = latest;
    if (socket?.readyState === WebSocket.OPEN && connectionState === 'online') {
      socket.send(JSON.stringify({ type: 'read', last_read_message_id: latest }));
    } else {
      api.markRead(gid, latest).catch(() => {});
    }
  }

  function isNearBottom() {
    return scroll.scrollHeight - scroll.scrollTop - scroll.clientHeight < 100;
  }

  function updateScrollAffordance() {
    scroll.classList.toggle('is-away-from-bottom', !isNearBottom());
  }

  function renderAiMessages() {
    mount(aiBody, html`${aiMessages.map((message) => html`
      <div class="ai-message ${message.role}">${message.content}${message.meta ? html`<span class="badge badge-amber mt-8">${message.meta}</span>` : ''}</div>
    `)}${aiLoading ? html`<div class="ai-message assistant">${spinner('正在根据聚合账目思考…')}</div>` : ''}`);
    requestAnimationFrame(() => {
      aiBody.scrollTop = aiBody.scrollHeight;
    });
  }

  async function askAi(question) {
    if (aiLoading) {
      return;
    }
    aiMessages.push({ role: 'user', content: question });
    aiLoading = true;
    renderAiMessages();
    aiController?.abort();
    aiController = new AbortController();
    try {
      const result = await api.aiAsk(gid, question, { signal: aiController.signal });
      if (!active) {
        return;
      }
      aiMessages.push({
        role: 'assistant',
        content: result.answer,
        meta: result.cached ? '缓存结果' : result.elapsed_ms ? `${result.elapsed_ms}ms` : '',
      });
    } catch (error) {
      if (!active || error.aborted) {
        return;
      }
      aiMessages.push({ role: 'assistant', content: friendlyError(error) });
    } finally {
      aiLoading = false;
      renderAiMessages();
    }
  }

  async function loadAiSuggestion() {
    if (aiLoading) {
      return;
    }
    aiLoading = true;
    renderAiMessages();
    aiController?.abort();
    aiController = new AbortController();
    try {
      const result = await api.aiSuggestions(gid, { signal: aiController.signal });
      if (!active) {
        return;
      }
      aiMessages.push({
        role: 'assistant',
        content: result.answer,
        meta: result.cached ? '今日缓存' : '刚刚生成',
      });
    } catch (error) {
      if (!active || error.aborted) {
        return;
      }
      aiMessages.push({ role: 'assistant', content: friendlyError(error) });
    } finally {
      aiLoading = false;
      renderAiMessages();
    }
  }

  function wait(ms) {
    return new Promise((resolve) => window.setTimeout(resolve, ms));
  }

  function autoGrow(textarea, maxHeight) {
    textarea.style.height = 'auto';
    textarea.style.height = `${Math.min(textarea.scrollHeight, maxHeight)}px`;
  }

  return {
    cleanup() {
      active = false;
      messageController?.abort();
      aiController?.abort();
      window.clearTimeout(reconnectTimer);
      stopFallback();
      socket?.close(1000, 'leaving chat');
      socket = null;
    },
  };
}
