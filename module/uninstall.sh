#!/system/bin/sh
# 删除模块只清理运行时状态。
# dtbo 是分区级修改，与模块挂载无关，卸载不会自动还原。
# 需要还原请重刷本包并选择关闭全部功能，或用 obk-restore.zip 回刷备份。
rm -f /data/obk/dtbo.md5 /data/obk/service.log /data/obk/daemon.log
