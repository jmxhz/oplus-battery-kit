#!/system/bin/sh
# ============================================================================
#  oplus battery kit - 开机
#    1) 刷新深放计数，让新的策略表立即重算
#    2) 按配置恢复运行时开关
#    3) 校验 dtbo 分区是否仍是安装时写入的内容
#    4) 结果写回 module.prop，可在管理器里直接看到
#    5) 按需拉起恒压涓流守护
# ============================================================================

MODDIR=${0%/*}
OBK="$MODDIR/bin/obk"
PROF="$MODDIR/profiles"
DATA=/data/obk
LOG="$DATA/service.log"

until [ "$(getprop sys.boot_completed)" = "1" ]; do sleep 5; done
sleep 8

mkdir -p "$DATA"
[ -f "$LOG" ] && [ "$(stat -c %s "$LOG" 2>/dev/null || echo 0)" -gt 262144 ] && : > "$LOG"
log() { echo "[$(date '+%F %T')] $*" >> "$LOG"; }
log "oplus battery kit  https://github.com/SkyBlue997/oplus-battery-kit  免费开源"

chmod 0755 "$OBK" 2>/dev/null
O="$OBK --profiles $PROF --root $DATA"
[ -x "$OBK" ] || { log "obk 不可执行"; exit 1; }

get() { $O cfg get "$1" 2>/dev/null; }
on()  { [ "$(get "$1")" = "1" ]; }

# --- 1. 深放计数刷新 -------------------------------------------------------
if $O batt refresh >/dev/null 2>&1; then
    log "深放计数已刷新"
else
    log "深放计数节点不可用"
fi

# --- 2. 运行时开关 ---------------------------------------------------------
# 新内核的 votable 协议开关：先激活 force 通道并置为开启(0)
for d in PPS_DISABLE UFCS_DISABLE VOOC_DISABLE; do
    [ -e "/proc/oplus-votable/$d/force_active" ] && echo 1 > "/proc/oplus-votable/$d/force_active" 2>/dev/null
    [ -e "/proc/oplus-votable/$d/force_val" ] && echo 0 > "/proc/oplus-votable/$d/force_val" 2>/dev/null
done

for p in pps ufcs svooc; do
    v="$(get "proto_$p")"
    [ -z "$v" ] && v=1
    if $O batt proto "$p" "$([ "$v" = "1" ] && echo on || echo off)" >/dev/null 2>&1; then
        log "协议 $p 设为 $v"
    else
        log "协议 $p 无可写节点，已跳过"
    fi
done

v="$(get lock_votes)"; [ -z "$v" ] && v=1
if [ "$v" = "1" ]; then
    $O batt lockvotes on >/dev/null 2>&1 && log "电流投票已锁定"
fi

# 解除超级省电模式对关机电压的投票 (2900mV -> 2750mV)
# 仅跟随 ddrc 开关：开启时才写回 1，关闭时保持原厂投票行为。
SE=/sys/class/oplus_chg/common/super_endurance_mode_status
if on ddrc && [ -e "$SE" ]; then
    echo 1 > "$SE" 2>/dev/null && log "超级省电模式截止电压已解除" || log "超级省电模式节点不可写"
fi

if on fake_temp; then
    $O batt faketemp on >/dev/null 2>&1 && log "温度伪装已开启"
fi

# UFCS 协议认证数据，载荷缺失或开关关闭时跳过
v="$(get auth_ufcs)"; [ -z "$v" ] && v=1
if [ "$v" = "1" ] && [ -f "$PROF/auth_ufcs.bin" ]; then
    $O batt auth ufcs >/dev/null 2>&1 && log "ufcs 认证已下发" || log "ufcs 认证下发失败"
fi

# --- 3. dtbo 一致性 --------------------------------------------------------
STATUS=""
SLOT="$(getprop ro.boot.slot_suffix)"
PART="/dev/block/bootdevice/by-name/dtbo${SLOT}"
if [ -f "$DATA/dtbo.md5" ] && [ -e "$PART" ]; then
    want="$(cat "$DATA/dtbo.md5")"
    have="$(md5sum "$PART" | awk '{print $1}')"
    [ "$want" != "$have" ] && STATUS="dtbo 已被改动(可能是系统更新)，请重新应用配置 | "
fi

# --- 4. 生效判定 -----------------------------------------------------------
UV="$($O --json batt status 2>/dev/null | grep -o '"vbat_uv": *-\{0,1\}[0-9]*' | awk '{print $2}')"
FCC="$($O --json batt status 2>/dev/null | grep -o '"fcc": *-\{0,1\}[0-9]*' | awk '{print $2}')"
CC="$($O --json batt status 2>/dev/null | grep -o '"cycle_count": *-\{0,1\}[0-9]*' | awk '{print $2}')"
ACTIVE="$($O --json prof list 2>/dev/null | grep -o '"active":true' | wc -l | tr -d ' ')"

if [ -n "$UV" ] && [ "$UV" -gt 0 ] 2>/dev/null; then
    if on ddrc; then
        if [ "$UV" -le 2800 ]; then STATUS="${STATUS}降容策略已解除"
        else STATUS="${STATUS}未生效(截止 ${UV}mV)，断开充电器后重启"; fi
    else
        STATUS="${STATUS}官方参数"
    fi
    STATUS="$STATUS  截止=${UV}mV FCC=${FCC:-?}mAh 循环=${CC:-?} 生效段=${ACTIVE:-0}"
else
    STATUS="${STATUS}读不到电池节点，可能非该充电框架机型"
fi
log "$STATUS"

# --- 5. module.prop --------------------------------------------------------
# 管理器列表里那行描述是最常被看到的位置，顺带带上项目地址
[ -f "$MODDIR/module.prop.bak" ] && cp -f "$MODDIR/module.prop.bak" "$MODDIR/module.prop"
sed -i "s#^description=.*#description=${STATUS}  |  免费开源 github.com/SkyBlue997/oplus-battery-kit#" \
    "$MODDIR/module.prop"

# --- 6. 恒压守护 -----------------------------------------------------------
if on cv_daemon; then
    log "启动恒压涓流守护"
    $O daemon start >/dev/null 2>&1 &
fi
