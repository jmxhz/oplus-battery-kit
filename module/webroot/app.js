/* oplus battery kit - WebUI
 * 全部数据来自 obk 的 --json 输出，界面不做任何本地推断。
 */
'use strict';

const MOD  = '/data/adb/modules/oplus_batt_kit';
const OBK  = MOD + '/bin/obk --profiles ' + MOD + '/profiles --root /data/obk';
const POLL = 2000;
const KEEP = 150;

/* ------------------------------------------------------------ 执行层 -- */

let seq = 0;
function shell(cmd) {
  return new Promise((resolve) => {
    if (typeof ksu === 'undefined' || !ksu.exec) {
      resolve({ code: -1, stdout: '', stderr: '当前环境没有提供命令执行接口' });
      return;
    }
    const cb = '__obk_cb_' + (seq++);
    window[cb] = (code, stdout, stderr) => {
      delete window[cb];
      resolve({ code: Number(code), stdout: stdout || '', stderr: stderr || '' });
    };
    try {
      ksu.exec(cmd, '{}', cb);
    } catch (e) {
      delete window[cb];
      resolve({ code: -1, stdout: '', stderr: String(e) });
    }
  });
}

async function obk(args) { return shell(OBK + ' ' + args); }

/* 拼进 root shell 之前的白名单校验。
   使用者本身即 root，这里不是权限边界，而是防止一个非法字符
   静默改写配置或执行到别的命令上。 */
const SAFE_ID  = /^[A-Za-z0-9_]{1,48}$/;
const SAFE_NUM = /^-?\d{1,12}$/;
function okId(v)  { return typeof v === 'string' && SAFE_ID.test(v); }
function okNum(v) { return typeof v === 'string' && SAFE_NUM.test(v); }

async function obkJson(args) {
  const r = await obk('--json ' + args);
  if (r.code !== 0 && !r.stdout.trim()) return null;
  try { return JSON.parse(r.stdout); } catch (e) { return null; }
}

/* ------------------------------------------------------------ 工具 ---- */

const $  = (s) => document.querySelector(s);
const $$ = (s) => Array.from(document.querySelectorAll(s));

function el(tag, cls, txt) {
  const n = document.createElement(tag);
  if (cls) n.className = cls;
  if (txt !== undefined && txt !== null) n.textContent = txt;
  return n;
}

let toastTimer = null;
function toast(msg) {
  const t = $('#toast');
  t.textContent = msg;
  t.hidden = false;
  clearTimeout(toastTimer);
  toastTimer = setTimeout(() => { t.hidden = true; }, 2600);
}

function confirmBox(title, body) {
  return new Promise((resolve) => {
    $('#modal-title').textContent = title;
    $('#modal-body').textContent = body;
    $('#modal').hidden = false;
    const done = (v) => {
      $('#modal').hidden = true;
      $('#modal-yes').onclick = null;
      $('#modal-no').onclick = null;
      resolve(v);
    };
    $('#modal-yes').onclick = () => done(true);
    $('#modal-no').onclick  = () => done(false);
  });
}

const num = (v) => (typeof v === 'number' && v > -1000000 ? v : null);
/* obk 用 -1 表示读不到；电流与温度本身可以为负，用 signed 区分 */
const fmt = (v, unit, scale, signed) => {
  const n = num(v);
  if (n === null) return '--';
  if (!signed && n < 0) return '--';
  if (signed && n === -1) return '--';
  const x = scale ? n / scale : n;
  return (scale ? x.toFixed(1) : String(x)) + (unit || '');
};
const absOr = (v) => {
  const n = num(v);
  if (n === null || n === -1) return null;
  return Math.abs(n);
};

/* ------------------------------------------------------------ 状态 ---- */

const S = {
  status: null,
  detect: null,
  sections: [],
  cfg: {},
  pending: {},
  series: { power: [], current: [], voltage: [], temp: [] },
  chartKey: 'power',
  timer: null,
};

/* ------------------------------------------------------------ 监控 ---- */

const METRICS = [
  { k: 'vbat_mv',    t: '电压',   u: 'mV' },
  { k: 'current_ma', t: '电流',   u: 'mA', calc: (s) => absOr(s.current_ma), fix: 0 },
  { k: 'power',      t: '功率',   u: 'W', calc: (s) => {
      const v = num(s.vbat_mv), i = absOr(s.current_ma);
      return (v && v > 0 && i !== null) ? (v * i) / 1e6 : null; }, fix: 2 },
  { k: 'temp_dc',    t: '温度',   u: 'C', calc: (s) => {
      const n = num(s.temp_dc); return (n === null || n <= -1000) ? null : n / 10; }, fix: 1 },
  { k: 'fcc',        t: 'FCC',    u: 'mAh' },
  { k: 'rm',         t: '剩余',   u: 'mAh' },
  { k: 'cycle_count',t: '循环',   u: '' },
  { k: 'vbat_uv',    t: '关机电压', u: 'mV' },
];

function renderMetrics(s) {
  const box = $('#metrics');
  box.textContent = '';
  METRICS.forEach((m) => {
    const c = el('div', 'cell');
    c.appendChild(el('u', null, m.t));
    const b = el('b');
    let v;
    if (m.calc) {
      const x = m.calc(s);
      v = x === null ? '--' : x.toFixed(m.fix === undefined ? 1 : m.fix);
    } else {
      v = fmt(s[m.k], '', m.scale);
    }
    b.textContent = v;
    if (m.u) { const i = el('i', null, m.u); b.appendChild(i); }
    c.appendChild(b);
    box.appendChild(c);
  });
}

const INFO = [
  ['电池型号', (s) => s.battery_type || '--'],
  ['生产日期', (s) => s.manu_date || '--'],
  ['健康度',   (s) => fmt(s.soh, '')],
  ['真实电量', (s) => fmt(s.real_soc, '%')],
  ['深放计数', (s) => fmt(s.deep_dischg, '')],
  ['充电状态', (s) => s.status || '--'],
  ['充电器',   (s) => (s.usb_online === 1 ? '已连接' : '未连接')],
  ['协商功率', (s) => fmt(s.cpa_power, ' W')],
  ['电流投票', (s) => fmt(s.bcc_current, ' mA')],
  ['充满预计', (s) => (num(s.time_to_full) > 0
      ? Math.round(s.time_to_full / 60) + ' 分钟' : '--')],
];

function renderInfo(s) {
  const dl = $('#battinfo');
  dl.textContent = '';
  INFO.forEach(([k, f]) => {
    dl.appendChild(el('dt', null, k));
    dl.appendChild(el('dd', null, f(s)));
  });
}

const NS_SVG = 'http://www.w3.org/2000/svg';
function svgEl(t, a) {
  const n = document.createElementNS(NS_SVG, t);
  for (const k in a) n.setAttribute(k, a[k]);
  return n;
}

/* 刻度标尺：每 2% 一根短刻线，每 10% 一根长刻线，实线段表示当前电量 */
function drawScale(pct) {
  const svg = $('#scale');
  if (!svg) return;
  svg.textContent = '';
  const W = 600, H = 26, BASE = 20;
  svg.appendChild(svgEl('line', { class: 'base', x1: 0, y1: BASE, x2: W, y2: BASE }));
  for (let p = 0; p <= 100; p += 2) {
    const x = (W * p) / 100;
    const major = p % 10 === 0;
    svg.appendChild(svgEl('line', {
      class: 'tick' + (major ? ' major' : ''),
      x1: x, y1: BASE, x2: x, y2: BASE - (major ? 9 : 4)
    }));
  }
  const w = (W * Math.max(0, Math.min(100, pct))) / 100;
  if (w > 0) svg.appendChild(svgEl('line', { class: 'fill', x1: 0, y1: BASE, x2: w, y2: BASE }));
}

function renderRing(s) {
  const soc = num(s.soc);
  const pct = soc === null || soc < 0 ? 0 : Math.min(100, soc);
  $('#m-soc').textContent = soc === null || soc < 0 ? '--' : String(soc);
  drawScale(pct);
  const i = absOr(s.current_ma);
  $('#m-status').textContent = s.usb_online === 1 ? '充电中' : '放电中';
  const sub = $('#m-sub');
  if (sub) sub.textContent = i === null ? '--' : i + ' mA';
}

function pushSeries(s) {
  const v = num(s.vbat_mv), i = absOr(s.current_ma), t = num(s.temp_dc);
  const p = (v !== null && v > 0 && i !== null) ? (v * i) / 1e6 : null;
  const add = (k, x) => {
    if (x === null) return;
    S.series[k].push(x);
    if (S.series[k].length > KEEP) S.series[k].shift();
  };
  add('power', p);
  add('current', i);
  add('voltage', v);
  add('temp', (t === null || t <= -1000) ? null : t / 10);
}

const CHART_UNIT = { power: 'W', current: 'mA', voltage: 'mV', temp: 'C' };

function drawChart() {
  const svg = $('#chart');
  const data = S.series[S.chartKey];
  svg.textContent = '';
  const W = 640, H = 220, PL = 46, PR = 8, PT = 12, PB = 24;

  const mk = svgEl;

  if (data.length < 2) {
    const tx = mk('text', { x: W / 2, y: H / 2, 'text-anchor': 'middle', class: 'lbl' });
    tx.textContent = '正在采集';
    svg.appendChild(tx);
    return;
  }

  let lo = Math.min.apply(null, data), hi = Math.max.apply(null, data);
  if (hi - lo < 1e-6) { hi = lo + 1; }
  const pad = (hi - lo) * 0.12;
  lo -= pad; hi += pad;

  const X = (i) => PL + (W - PL - PR) * (i / (data.length - 1));
  const Y = (v) => PT + (H - PT - PB) * (1 - (v - lo) / (hi - lo));

  for (let g = 0; g <= 3; g++) {
    const v = lo + (hi - lo) * (g / 3);
    const y = Y(v);
    svg.appendChild(mk('line', { class: 'axis', x1: PL, y1: y, x2: W - PR, y2: y }));
    const tx = mk('text', { class: 'lbl', x: PL - 6, y: y + 4, 'text-anchor': 'end' });
    tx.textContent = Math.abs(v) >= 100 ? v.toFixed(0) : v.toFixed(1);
    svg.appendChild(tx);
  }

  let d = '', a = '';
  data.forEach((v, i) => {
    const x = X(i).toFixed(1), y = Y(v).toFixed(1);
    d += (i ? 'L' : 'M') + x + ' ' + y + ' ';
  });
  a = d + 'L' + X(data.length - 1).toFixed(1) + ' ' + (H - PB) +
      ' L' + PL + ' ' + (H - PB) + ' Z';
  svg.appendChild(mk('path', { class: 'area', d: a }));
  svg.appendChild(mk('path', { class: 'line', d: d }));
  svg.appendChild(mk('circle', {
    class: 'dot', cx: X(data.length - 1), cy: Y(data[data.length - 1]), r: 3.2 }));

  const u = mk('text', { class: 'lbl', x: W - PR, y: H - 6, 'text-anchor': 'end' });
  u.textContent = CHART_UNIT[S.chartKey] + ' - 最近 ' + data.length + ' 点';
  svg.appendChild(u);
}

/* ------------------------------------------------------------ 功能 ---- */

function sectionRow(sec) {
  const row = el('div', 'row');
  const main = el('div', 'row-main');
  const title = el('div', 'row-title');
  title.appendChild(el('strong', null, sec.title || sec.id));
  if (sec.force)   title.appendChild(el('span', 'pill lock', '模块必需'));
  if (sec.active)  title.appendChild(el('span', 'pill live', '已生效'));
  if (!sec.runtime && S.pending[sec.id] !== undefined &&
      S.pending[sec.id] !== sec.enabled)
    title.appendChild(el('span', 'pill chg', '待应用'));
  main.appendChild(title);

  if (sec.warn) main.appendChild(el('div', 'row-warn', sec.warn));
  if (sec.suggest) {
    const s = sec.suggest.trim();
    const dep = S.sections.find((x) => x.id === s);
    if (dep && !dep.enabled)
      main.appendChild(el('div', 'row-warn',
        '建议同时启用: ' + (dep.title || s)));
  }
  row.appendChild(main);

  const sw = el('button', 'sw');
  const cur = S.pending[sec.id] !== undefined ? S.pending[sec.id] : sec.enabled;
  sw.setAttribute('role', 'switch');
  sw.setAttribute('aria-checked', String(!!cur));
  sw.setAttribute('aria-label', sec.title || sec.id);
  if (sec.force) sw.disabled = true;
  sw.onclick = () => onToggle(sec, sw);
  row.appendChild(sw);
  return row;
}

async function onToggle(sec, sw) {
  const cur = sw.getAttribute('aria-checked') === 'true';
  const next = !cur;
  sw.setAttribute('aria-checked', String(next));

  if (sec.runtime) {
    if (!okId(sec.id)) { toast('功能标识非法，已忽略'); sw.setAttribute('aria-checked', String(cur)); return; }
    sw.disabled = true;
    const v = next ? '1' : '0';
    await obk('cfg set ' + sec.id + '=' + v);
    let r = { code: 0 };
    if (sec.id.indexOf('proto_') === 0) {
      r = await obk('batt proto ' + sec.id.slice(6) + ' ' + (next ? 'on' : 'off'));
    } else if (sec.id === 'fake_temp') {
      r = await obk('batt faketemp ' + (next ? 'on' : 'off'));
    } else if (sec.id === 'lock_votes') {
      r = await obk('batt lockvotes ' + (next ? 'on' : 'off'));
    } else if (sec.id === 'cv_daemon') {
      r = next ? await shell('nohup ' + OBK + ' daemon start >/dev/null 2>&1 &')
               : await obk('daemon stop');
      toast(next ? '守护已启动' : '守护已停止');
    }
    sw.disabled = false;
    if (r.code !== 0) {
      toast('设置失败: ' + (r.stderr || '节点不可用').split('\n')[0]);
      sw.setAttribute('aria-checked', String(cur));
      await obk('cfg set ' + sec.id + '=' + (cur ? '1' : '0'));
    } else {
      toast((sec.title || sec.id) + (next ? ' 已开启' : ' 已关闭'));
    }
    await loadSections();
    return;
  }

  S.pending[sec.id] = next;
  renderSections();
  updateApplyBar();
}

function updateApplyBar() {
  let n = 0;
  S.sections.forEach((s) => {
    if (!s.runtime && S.pending[s.id] !== undefined && S.pending[s.id] !== s.enabled) n++;
  });
  $('#apply').disabled = n === 0;
  $('#dirty-hint').textContent = n === 0
    ? '没有待应用的改动'
    : '有 ' + n + ' 项改动待应用';
}

function renderSections() {
  const d = $('#dtbo-list'), r = $('#rt-list');
  d.textContent = ''; r.textContent = '';
  S.sections.forEach((s) => {
    (s.runtime ? r : d).appendChild(sectionRow(s));
  });
}

async function loadSections() {
  const j = await obkJson('prof list');
  if (!j) return;
  S.sections = j.sections || [];
  $('#subtitle').textContent = j.device
    ? j.device + ' - ' + S.sections.length + ' 项功能'
    : '未识别机型';
  Object.keys(S.pending).forEach((k) => {
    const s = S.sections.find((x) => x.id === k);
    if (s && s.enabled === S.pending[k]) delete S.pending[k];
  });
  renderSections();
  updateApplyBar();
}

async function doApply() {
  const ids = Object.keys(S.pending);
  if (!ids.length) return;
  const ok = await confirmBox('应用并重启',
    '将把 ' + ids.length + ' 项改动写入 dtbo 分区，随后需要重启才会生效。' +
    '写入过程中请勿关机。');
  if (!ok) return;

  $('#apply').disabled = true;
  $('#dirty-hint').textContent = '正在写入';
  if (!ids.every(okId)) { toast('存在非法功能标识，已中止'); $('#apply').disabled = false; return; }
  const sets = ids.map((k) => k + '=' + (S.pending[k] ? 1 : 0)).join(' ');
  await obk('cfg set ' + sets);
  const r = await obk('prof apply');
  if (r.code !== 0) {
    toast('应用失败，详见系统页日志');
    appendLog(r.stdout + r.stderr);
    $('#dirty-hint').textContent = '应用失败';
    $('#apply').disabled = false;
    return;
  }
  S.pending = {};
  await loadSections();
  const rb = await confirmBox('已写入', '改动已写入 dtbo 分区。现在重启？重启前请拔掉充电器。');
  if (rb) await shell('svc power reboot || reboot');
}

/* ------------------------------------------------------------ 恒压 ---- */

const CV = [
  ['cv_vol_mv',                '恒压目标电压', 'mV', 4565],
  ['cv_max_ma',                '恒压段限流',   'mA', 5000],
  ['cv_ufcs_max_ma',           'UFCS 最大电流','mA', 9100],
  ['cv_pps_max_ma',            'PPS 最大电流', 'mA', 5000],
  ['cv_inc_step_ma',           '升流步长',     'mA', 100],
  ['cv_dec_step_ma',           '降流步长',     'mA', 100],
  ['cv_tc_vol_thr_mv',         '进入涓流电压', 'mV', 4500],
  ['cv_tc_thr_soc',            '进入涓流电量', '%',  98],
  ['cv_tc_full_ma',            '判满电流',     'mA', 400],
  ['cv_tc_vol_full_mv',        '判满电压',     'mV', 4485],
  ['cv_batt_full_thr_mv',      '满电电压阈值', 'mV', 4570],
  ['cv_rise_quickstep_thr_mv', '快升上限电压', 'mV', 4250],
  ['cv_rise_wait_thr_mv',      '满速区上限',   'mV', 3800],
  ['cv_curr_inc_wait_cycles',  '升流等待圈数', '圈',  4],
  ['cv_loop_ms',               '主循环周期',   'ms', 2000],
  ['lock_votes_ma',            '投票锁定电流', 'mA', 13700],
  ['fake_temp_milli_c',        '伪装温度',     '毫度', 36000],
];

function renderCv() {
  const f = $('#cv-form');
  f.textContent = '';
  CV.forEach(([k, t, u, d]) => {
    const lb = el('label');
    lb.appendChild(document.createTextNode(t));
    lb.appendChild(el('em', null, k + ' (' + u + ')'));
    const inp = el('input');
    inp.type = 'number';
    inp.id = 'cv-' + k;
    inp.value = S.cfg[k] !== undefined ? S.cfg[k] : d;
    f.appendChild(lb);
    f.appendChild(inp);
  });
}

async function saveCv() {
  const parts = [];
  for (const [k, title] of CV) {
    const v = $('#cv-' + k).value.trim();
    if (v === '') continue;
    if (!okNum(v)) { toast('“' + title + '” 需要整数'); $('#cv-' + k).focus(); return; }
    if (!okId(k))  { toast('参数名非法: ' + k); return; }
    parts.push(k + '=' + v);
  }
  if (!parts.length) { toast('没有可保存的项'); return; }
  const r = await obk('cfg set ' + parts.join(' '));
  if (r.code !== 0) { toast('保存失败'); return; }
  const pid = await obkJson('daemon status');
  toast(pid && pid.running ? '已保存，重启守护后生效' : '已保存');
  await loadCfg();
}

async function loadCfg() {
  const j = await obkJson('cfg get');
  S.cfg = j || {};
  renderCv();
}

/* ------------------------------------------------------------ 系统 ---- */

async function loadSystem() {
  const d = await obkJson('avb detect');
  S.detect = d;
  const dl = $('#detect');
  dl.textContent = '';
  const rows = d ? [
    ['AVB 校验',  d.verification === 'enabled' ? '开启' :
                  d.verification === 'disabled' ? '已关闭' : '未知'],
    ['dm-verity', d.verity === 'enabled' ? '开启' :
                  d.verity === 'disabled' ? '已关闭' : '未知'],
    ['dtbo 形态', d.dtbo_form],
    ['处理模式',  d.mode + (d.mode_locked ? ' (自动判定)' : '')],
    ['引导状态',  d.bl_state || '--'],
    ['fastboot 回刷', d.fastboot_rescue ? '可用' : '不可用'],
    ['属性伪装',  d.spoof_detected ? '检测到' : '无'],
  ] : [['状态', '读取失败']];
  rows.forEach(([k, v]) => {
    dl.appendChild(el('dt', null, k));
    dl.appendChild(el('dd', null, String(v)));
  });

  const sj = await obkJson('snap info');
  const sd = $('#snap');
  sd.textContent = '';
  const srows = sj ? [
    ['机型', sj.device],
    ['来源', sj.source === 'stock' ? '官方镜像' : '安装时分区'],
    ['分区大小', sj.partsize + ' 字节'],
    ['属性快照', sj.props + ' 条'],
    ['子树快照', sj.trees + ' 条'],
    ['版本匹配', sj.match ? '是' : '否（系统已更新）'],
  ] : [['状态', '尚无快照']];
  srows.forEach(([k, v]) => {
    sd.appendChild(el('dt', null, k));
    sd.appendChild(el('dd', null, String(v)));
  });

  const lg = await shell('tail -n 60 /data/obk/service.log 2>/dev/null');
  $('#syslog').textContent = lg.stdout.trim() || '暂无日志';
  const cl = await shell('tail -n 60 /data/obk/daemon.log 2>/dev/null');
  const ds = await obkJson('daemon status');
  const head = ds ? (ds.running ? '守护运行中 pid ' + ds.pid : '守护未运行') : '';
  $('#cv-log').textContent = (head ? head + '\n\n' : '') + (cl.stdout.trim() || '暂无日志');
}

async function doRevert() {
  const ok = await confirmBox('全部还原',
    '将关闭所有可选功能，并把 dtbo 中被本模块改动的属性与节点全部写回原厂值。' +
    '完成后需要重启。');
  if (!ok) return;
  toast('正在还原');
  const r = await obk('prof revert');
  if (r.code !== 0) { toast('还原失败'); appendLog(r.stdout + r.stderr); return; }
  await loadSections();
  const rb = await confirmBox('已还原', '现在重启？');
  if (rb) await shell('svc power reboot || reboot');
}

function appendLog(txt) {
  const p = $('#syslog');
  p.textContent = (txt || '').trim() + '\n\n' + p.textContent;
}

/* ------------------------------------------------------------ 轮询 ---- */

function banner() {
  const b = $('#banner');
  const s = S.status;
  const ddrc = S.sections.find((x) => x.id === 'ddrc');
  if (!s) { b.hidden = true; return; }
  if (num(s.vbat_uv) === null || s.vbat_uv < 0) {
    b.hidden = false; b.className = 'banner warn';
    b.textContent = '读不到电池节点，本机可能不是该充电框架机型。';
    return;
  }
  if (ddrc && ddrc.enabled && s.vbat_uv > 2800) {
    b.hidden = false; b.className = 'banner bad';
    b.textContent = '降容策略尚未生效（当前截止 ' + s.vbat_uv +
      ' mV）。请断开充电器后重启。';
    return;
  }
  if (ddrc && ddrc.enabled) {
    b.hidden = false; b.className = 'banner ok';
    b.textContent = '降容策略已解除，放电截止电压 ' + s.vbat_uv + ' mV。';
    return;
  }
  b.hidden = true;
}

async function tick() {
  const s = await obkJson('batt status');
  if (s) {
    S.status = s;
    renderRing(s);
    renderMetrics(s);
    renderInfo(s);
    pushSeries(s);
    drawChart();
    banner();
  }
}

/* ------------------------------------------------------------ 启动 ---- */

function bindTabs() {
  $$('.tab').forEach((t) => {
    t.onclick = () => {
      $$('.tab').forEach((x) => x.classList.remove('is-on'));
      $$('.view').forEach((x) => x.classList.remove('is-on'));
      t.classList.add('is-on');
      $('#view-' + t.dataset.view).classList.add('is-on');
      if (t.dataset.view === 'system') loadSystem();
      if (t.dataset.view === 'charge') loadCfg();
    };
  });
  $$('#chart-pick button').forEach((b) => {
    b.onclick = () => {
      $$('#chart-pick button').forEach((x) => x.classList.remove('on'));
      b.classList.add('on');
      S.chartKey = b.dataset.k;
      drawChart();
    };
  });
}

async function boot() {
  bindTabs();
  $('#refresh').onclick = async () => {
    await loadSections();
    await tick();
    toast('已刷新');
  };
  $('#apply').onclick   = doApply;
  $('#repo-copy').onclick = async () => {
    const url = $('#repo-url').textContent.trim();
    let ok = false;
    try {
      if (navigator.clipboard && navigator.clipboard.writeText) {
        await navigator.clipboard.writeText(url);
        ok = true;
      }
    } catch (e) { ok = false; }
    if (!ok) {
      /* WebView 里 clipboard API 常不可用，退回旧接口 */
      const ta = document.createElement('textarea');
      ta.value = url;
      ta.style.position = 'fixed';
      ta.style.opacity = '0';
      document.body.appendChild(ta);
      ta.select();
      try { ok = document.execCommand('copy'); } catch (e) { ok = false; }
      document.body.removeChild(ta);
    }
    if (!ok) {
      /* 两条路都不通时，直接把地址选中，用户长按即可走系统复制 */
      try {
        const rg = document.createRange();
        rg.selectNodeContents($('#repo-url'));
        const sel = window.getSelection();
        sel.removeAllRanges();
        sel.addRange(rg);
      } catch (e) { /* 选中失败不影响提示 */ }
    }
    toast(ok ? '地址已复制' : '已选中地址，长按后选复制');
  };
  $('#revert').onclick  = doRevert;
  $('#cv-save').onclick = saveCv;
  $('#cv-reset').onclick = async () => {
    CV.forEach(([k, , , d]) => { $('#cv-' + k).value = d; });
    toast('已填回默认值，仍需点保存');
  };

  const probe = await obk('version');
  const av = $('#about-ver');
  if (av) av.textContent = probe.code === 0
    ? probe.stdout.trim().replace(/^obk\s+/, 'v') : '--';
  if (probe.code !== 0) {
    $('#subtitle').textContent = '无法调用 obk';
    $('#banner').hidden = false;
    $('#banner').className = 'banner bad';
    $('#banner').textContent =
      '无法执行模块程序。请确认在支持 WebUI 的管理器中打开，且模块已正确安装。';
    return;
  }

  await loadSections();
  await loadCfg();
  await tick();
  S.timer = setInterval(tick, POLL);
  document.addEventListener('visibilitychange', () => {
    clearInterval(S.timer);
    if (!document.hidden) { tick(); S.timer = setInterval(tick, POLL); }
  });
}

document.addEventListener('DOMContentLoaded', boot);
