# 安装期界面与按键读取

BAR="-------------------------------------------------"

ui_br()  { ui_print "$BAR"; }
ui_nl()  { ui_print " "; }
ui_kv()  { ui_print "$(printf '  %-12s %s' "$1" "$2")"; }

# 从输入子系统读一次按键，回显 up / down / power
read_key() {
    local ev type code state
    while :; do
        ev="$(getevent -qlc 1 2>/dev/null)"
        [ -n "$ev" ] || continue
        type="$(echo "$ev" | awk '{print $2}')"
        [ "$type" = "EV_KEY" ] || continue
        state="$(echo "$ev" | awk '{print $4}')"
        [ "$state" = "DOWN" ] || continue
        code="$(echo "$ev" | awk '{print $3}')"
        case "$code" in
            KEY_VOLUMEUP)   echo up;    return 0 ;;
            KEY_VOLUMEDOWN) echo down;  return 0 ;;
            KEY_POWER)      echo power; return 0 ;;
        esac
    done
}

# ask2 "问题" "音量+ 含义" "音量- 含义" -> 0 表示选了音量+
ask2() {
    ui_br
    ui_nl
    ui_print "  $1"
    ui_print "    音量+   $2"
    ui_print "    音量-   $3"
    ui_nl
    case "$(read_key)" in
        up)   ui_print "  已选择: $2"; ui_br; return 0 ;;
        *)    ui_print "  已选择: $3"; ui_br; return 1 ;;
    esac
}

# ask3 "问题" "+含义" "-含义" "电源含义" -> 回显 up/down/power
ask3() {
    ui_br
    ui_nl
    ui_print "  $1"
    ui_print "    音量+   $2"
    ui_print "    音量-   $3"
    ui_print "    电源键  $4"
    ui_nl
    local k
    k="$(read_key)"
    case "$k" in
        up)    ui_print "  已选择: $2" ;;
        down)  ui_print "  已选择: $3" ;;
        power) ui_print "  已选择: $4" ;;
    esac
    ui_br
    echo "$k"
}

# 仅提示，等待确认
ack() {
    ui_br
    ui_nl
    ui_print "  $1"
    ui_nl
    ui_print "    音量+   继续"
    ui_print "    音量-   退出安装"
    ui_nl
    [ "$(read_key)" = "up" ] || abort_clean "已取消"
}

abort_clean() {
    ui_nl
    ui_print "  $1"
    ui_print "  安装中止，dtbo 分区未被写入"
    ui_nl
    exit 1
}
