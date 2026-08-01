#!/system/bin/sh
# ============================================================================
#  oplus battery kit - 安装流程
#
#  刷入 -> 识别机型 -> 快照原厂值 -> 导出救砖备份 -> 选择功能 -> 应用
#  之后所有功能都可在 WebUI 里勾选并一键应用，无需再刷本包。
# ============================================================================

[ -f "$MODPATH/lib/ui.sh" ] || abort "! 缺少 lib/ui.sh"
. "$MODPATH/lib/ui.sh"

OBK="$MODPATH/bin/obk"
PROF="$MODPATH/profiles"
DATA=/data/obk
BACKUP_DIR=/sdcard/obk_backup
STOCK_IMG=/sdcard/dtbo_stock.img

chmod 0755 "$OBK" 2>/dev/null
[ -x "$OBK" ] || abort "! bin/obk 不可执行"

O="$OBK --profiles $PROF --root $DATA"

mkdir -p "$DATA" "$DATA/stock" "$BACKUP_DIR"

# ------------------------------------------------------------- 机型 -----
MODEL="$(getprop ro.product.vendor.name)"
[ -n "$MODEL" ] || MODEL="$(getprop ro.product.name)"
FP="$(getprop ro.build.fingerprint)"
SLOT="$(getprop ro.boot.slot_suffix)"
PART="/dev/block/bootdevice/by-name/dtbo${SLOT}"

RULE="$(grep -E "^${MODEL}=" "$PROF/devices.map" 2>/dev/null | head -1 | cut -d= -f2)"

ui_nl
ui_print "  oplus battery kit"
ui_print "  https://github.com/SkyBlue997/oplus-battery-kit"
ui_nl
ui_print "  本模块完全免费、开源。"
ui_print "  若你是付费获得的，说明被转卖了，请去上面的地址自行获取。"
ui_nl
ui_kv "机型"     "${MODEL:-未知}"
ui_kv "规则"     "${RULE:-无}"
ui_kv "系统"     "$(getprop ro.build.version.release)"
ui_kv "分区"     "$PART"
ui_br

[ -n "$RULE" ] || abort_clean "不支持的机型 ${MODEL:-未知}"
[ -e "$PART" ] || abort_clean "找不到 dtbo 分区"
[ -f "$PROF/$RULE.rule" ] || abort_clean "缺少规则文件 $RULE.rule"

# --------------------------------------------------------- AVB 探测 -----
DET="$($O avb detect 2>&1)"
DET_RC=$?
echo "$DET" | while IFS= read -r l; do ui_print "  $l"; done
ui_br

MODE="$($O --json avb detect 2>/dev/null | grep -o '"mode": *"[a-z]*"' | cut -d'"' -f4)"
NEED_STOCK="$($O --json avb detect 2>/dev/null | grep -o '"requires_stock": *[a-z]*' | awk '{print $2}')"
DIRTY="$($O --json avb detect 2>/dev/null | grep -o '"snapshot_dirty": *[a-z]*' | awk '{print $2}')"

if [ "$DET_RC" -eq 11 ]; then
    abort_clean "引导或 AVB 状态不满足安装条件，详见上方说明"
fi

# ------------------------------------------------------- 空转往返 -------
ui_print "  正在校验 DTBO 容器..."
ST="$($O dtbo selftest 2>&1)"
ui_print "  $ST"
case "$ST" in
    *校验失败*) abort_clean "DTBO 容器空转往返未通过，拒绝继续" ;;
esac
ui_br

# --------------------------------------------------------- 告知 ---------
ui_nl
ui_print "  以下改动会无条件执行:"
ui_print "    移除 LCF 充电结束策略"
ui_print "      内核不再自行判满，充满后可能仍有小电流。"
ui_print "      建议同时启用恒压涓流守护接管末段。"
ui_nl
ui_br
ui_print "  免责声明"
ui_nl
ui_print "  本模块修改电池管理与充电控制行为，与设备制造商无任何关联，"
ui_print "  未获得其认可或授权。按现状提供，不附带任何担保。"
ui_nl
ui_print "  可能导致:"
ui_print "    电池加速衰减、鼓包、损坏乃至起火"
ui_print "    充电控制异常、过充或过热"
ui_print "    设备无法开机、数据丢失、硬件永久损坏"
ui_print "    制造商保修失效"
ui_nl
ui_print "  在法律允许的最大范围内，作者不对由此产生的任何直接或间接"
ui_print "  损害承担责任。是否使用由你自行决定并承担全部后果。"
ui_nl
ui_print "  完整条款见模块附带的 README 免责声明一节。"
ack "已阅读并接受上述条款，继续安装？"

# --------------------------------------------------------- 快照 ---------
SRC=live
if [ "$DIRTY" = "true" ]; then
    ui_nl
    ui_print "  检测到 dtbo 已被改动过。"
    ui_print "  直接快照会把已有改动当成原厂值。"
    if [ -f "$STOCK_IMG" ]; then
        ui_print "  发现 $STOCK_IMG，将改用它作为原厂基线。"
        SRC=stock
    else
        ack "未找到 $STOCK_IMG。继续将以当前分区为基线，还原后不等于出厂状态。"
    fi
fi

if [ "$SRC" = "stock" ]; then
    $O snap create --source stock --stock "$STOCK_IMG" || abort_clean "快照失败"
else
    $O snap create || abort_clean "快照失败"
fi
ui_print "  $($O snap info | tr '\n' ' ')"
ui_br

# ------------------------------------------------- 救砖备份（强制）-----
BK="$BACKUP_DIR/dtbo_${RULE}_$(echo "$FP" | tr -c 'A-Za-z0-9' '_' | cut -c1-40).img"
if [ ! -f "$BK" ]; then
    ui_print "  正在导出原厂 dtbo 到内置存储..."
    dd if="$PART" of="$BK" 2>/dev/null || abort_clean "导出备份失败"
fi
MD5="$(md5sum "$BK" | awk '{print $1}')"
echo "$MD5" > "$BK.md5"

cat > "$BACKUP_DIR/救砖说明.txt" <<EOF
本机 dtbo 原厂备份
==================

机型        : $MODEL ($RULE)
系统指纹    : $FP
分区        : $PART
备份文件    : $(basename "$BK")
MD5         : $MD5

如果刷入后无法开机：

1. 进入 fastboot（关机后音量下 + 电源，或 adb reboot bootloader）
2. 在电脑上确认能识别设备：
       fastboot devices
3. 回刷本备份：
       fastboot flash dtbo${SLOT} $(basename "$BK")
4. 重启：
       fastboot reboot

若装有第三方 recovery，也可以直接刷入 obk-restore.zip，
它会自动在本目录里找到与当前系统版本匹配的备份并写回。

务必先把本目录整个拷到电脑。手机进不了系统时，这里的文件就取不到了。
EOF

ui_kv "备份"  "$BK"
ui_kv "MD5"   "$MD5"
ui_nl
ui_print "  这份备份在手机内部。真变砖时取不出来。"
ack "请先把 $BACKUP_DIR 整个拷到电脑，然后继续。"

# --------------------------------------------------------- AVB 缓存 -----
if [ "$MODE" = "graft" ] || [ "$NEED_STOCK" = "true" ]; then
    if [ -f "$STOCK_IMG" ]; then
        $O avb cache --stock "$STOCK_IMG" || abort_clean "AVB 参数固化失败"
    else
        abort_clean "当前需要官方 dtbo 镜像。请把对应系统版本官方包里的 dtbo 改名为 dtbo_stock.img 放到内置存储根目录后重刷。"
    fi
else
    $O avb cache >/dev/null 2>&1
fi
ui_print "  AVB 参数已固化，之后改配置不再需要外部镜像"
ui_br

# --------------------------------------------------------- 选择 ---------
CHOICE="$(ask3 "选择安装方式" \
    "推荐配置（两次按键完成）" \
    "逐项选择（按功能逐个确认）" \
    "仅安装，暂不改 dtbo")"

apply_needed=1

case "$CHOICE" in
power)
    ui_nl
    ui_print "  已安装 obk 与 WebUI，dtbo 未改动。"
    ui_print "  重启后在 WebUI 里勾选功能并一键应用。"
    apply_needed=0
    ;;
up)
    $O cfg set ddrc=1 soc_smooth=1 chg_boost=1 batt_therm=0 \
               proto_pps=1 proto_ufcs=1 proto_svooc=1 lock_votes=1 \
               fake_temp=0 cv_daemon=0 >/dev/null
    ui_nl
    ui_print "  推荐配置将启用:"
    ui_print "    解除深放降容"
    ui_print "    显示真实电量"
    ui_print "    UFCS 与 PPS 功率解锁"
    ui_print "    移除 LCF（强制）"
    ui_print "    内核调度模式按探测结果自动决定"
    ui_print "  不启用: 温度墙、温度伪装、恒压守护"
    ui_nl
    if ask2 "是否同时抬高温度墙？" "抬高到 48/53 摄氏度" "保持原厂"; then
        $O cfg set batt_therm=1 fake_temp=1 >/dev/null
        ui_print "  已同时开启温度伪装（53 度档依赖它）"
    fi
    ;;
down)
    # 取全部非强制的 dtbo 段。必须走 JSON：文本表的「状态」列在没有段生效时是
    # 空的，安装时恰恰一个段都没生效，按列号取会一个都选不中。
    IDS="$($O --json prof list 2>/dev/null | tr '{' '\n' \
           | grep '"runtime":false' | grep -v '"force":true' \
           | sed -n 's/.*"id":"\([^"]*\)".*/\1/p')"
    TOTAL="$(echo "$IDS" | grep -c .)"
    # 列表读不到就别猜。你选逐项就是想自己定，此时替你开任何东西都不合适，
    # 退回「只装不改」，让你在 WebUI 里看清楚再决定。
    if [ "$TOTAL" -eq 0 ]; then
        ui_nl
        ui_print "  读不到功能列表，dtbo 保持原样。"
        ui_print "  重启后在 WebUI 里勾选功能并一键应用。"
        apply_needed=0
    fi
    IDX=0
    for id in $IDS; do
        IDX=$((IDX + 1))
        T="$($O prof show "$id" 2>/dev/null)"
        TITLE="$(echo "$T" | awk '/^标题/{$1="";print substr($0,2)}')"
        WARNTXT="$(echo "$T" | awk '/^警告/{$1="";print substr($0,2)}')"
        DEF="$(echo "$T" | awk '/^当前开关/{print $2}')"
        ui_nl
        ui_print "  [$IDX/$TOTAL] $TITLE"
        [ -n "$WARNTXT" ] && ui_print "        $WARNTXT"
        if ask2 "是否启用？" "启用" "保持关闭（默认 $DEF）"; then
            $O cfg set "$id=1" >/dev/null
        else
            $O cfg set "$id=0" >/dev/null
        fi
    done
    $O cfg set proto_pps=1 proto_ufcs=1 proto_svooc=1 lock_votes=1 >/dev/null
    ;;
esac

# --------------------------------------------------------- 应用 ---------
if [ "$apply_needed" -eq 1 ]; then
    ui_nl
    ui_print "  正在应用，请勿关机..."
    $O prof apply >"$DATA/apply.out" 2>&1
    APPLY_RC=$?
    while IFS= read -r l; do ui_print "  $l"; done < "$DATA/apply.out"
    if [ "$APPLY_RC" -ne 0 ]; then
        ui_nl
        ui_print "  应用未成功（退出码 $APPLY_RC），dtbo 保持原样。"
        ui_print "  可在 WebUI 里查看详情后重试。"
    elif ! $O --json prof list 2>/dev/null | grep -q '"active":true'; then
        ui_print "  没有任何段生效，请检查上方输出"
    fi
fi

# --------------------------------------------------------- 收尾 ---------
cp -f "$MODPATH/module.prop" "$MODPATH/module.prop.bak" 2>/dev/null
mkdir -p "$MODPATH/webroot"
set_perm_recursive "$MODPATH" 0 0 0755 0644
set_perm "$OBK" 0 0 0755

ui_nl
ui_br
ui_print "  安装完成"
ui_nl
if [ "$apply_needed" -eq 1 ]; then
    ui_print "  重启前请拔掉充电器，"
    ui_print "  深放计数刷新只在未充电时有意义。"
    ui_nl
fi
ui_print "  重启后自检:"
ui_print "    cat /sys/class/oplus_chg/battery/vbat_uv"
ui_print "  读数不高于 2800 表示降容策略已解除"
ui_nl
ui_print "  救砖备份: $BACKUP_DIR"
ui_nl
ui_print "  项目地址 https://github.com/SkyBlue997/oplus-battery-kit"
ui_print "  本模块免费开源，问题反馈与更新请以该地址为准。"
ui_br
ui_nl
