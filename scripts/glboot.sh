#!/system/bin/sh
# GhostLock adb 启动守护进程
# 由 adb shell 启动（继承正常的 adb session keyring，fscrypt keyring 的 possessor），
# 轮询 App 外部目录里的 gltrigger 文件；收到 "start" 后跑 exploit + serve。
# 参数 $1 = 工作目录（/sdcard/Android/data/<pkg>/files/ghost）
DIR="$1"
TMP=/data/local/tmp
TRIG="$DIR/gltrigger"
STAT="$DIR/glstatus"
HB="$DIR/glheartbeat"
LOG="$DIR/gldaemon.log"

log() { echo "$(date +%H:%M:%S) $*" >> "$LOG"; }

[ -d "$DIR" ] || exit 2
cd "$TMP" || exit 3

sync_bins() {
  for f in ghost1 glsrv gl-su; do
    [ -f "$DIR/$f" ] || { log "missing $f"; return 1; }
    if [ ! -f "$TMP/$f" ] || [ "$(md5sum "$TMP/$f" 2>/dev/null | awk '{print $1}')" != "$(md5sum "$DIR/$f" 2>/dev/null | awk '{print $1}')" ]; then
      cp "$DIR/$f" "$TMP/$f" && chmod 755 "$TMP/$f" || log "copy $f failed"
    fi
  done
  return 0
}

sync_bins
log "daemon ready pid=$$ dir=$DIR"
echo "idle" > "$STAT"
echo "$(date +%s)" > "$HB"

hb=$(date +%s)
while true; do
  now=$(date +%s)
  if [ $((now - hb)) -ge 10 ]; then
    echo "$now" > "$HB"
    hb=$now
  fi
  if [ -f "$TRIG" ]; then
    cmd=$(cat "$TRIG" 2>/dev/null)
    rm -f "$TRIG"
    log "trigger '$cmd'"
    case "$cmd" in
      start*)
        if pidof glsrv >/dev/null 2>&1; then
          echo "ready" > "$STAT"
          continue
        fi
        sync_bins
        rm -f "$TMP/prog.log" "$TMP/pls.log"
        # 等 UI/触摸 boost 衰减，KernelSnitch 的空 futex 计时需要低频率
        echo "settling" > "$STAT"
        sleep 8
        ok=0; attempt=0
        while [ $attempt -lt 6 ]; do
          attempt=$((attempt+1))
          rm -f "$TMP/rt.log"
          (cd "$TMP" && setsid ./ghost1 root > "$TMP/rt.log" 2>&1 < /dev/null) &
          gp=$!
          n=0
          while [ $n -lt 150 ]; do
            sleep 1
            n=$((n+1))
            if grep -q "\[rt\] FINAL uid=0" "$TMP/rt.log" 2>/dev/null; then ok=1; break; fi
            if grep -q -E "RT_FAIL|BAD slide|REJECT" "$TMP/rt.log" 2>/dev/null; then break; fi
            if ! kill -0 "$gp" 2>/dev/null && [ $n -gt 5 ]; then break; fi
          done
          cp -f "$TMP/rt.log" "$DIR/rt_att$attempt.log" 2>/dev/null
          [ $ok -eq 1 ] && break
          sleep 3
        done
        if [ $ok -ne 1 ]; then
          echo "exploit_failed" > "$STAT"
          log "exploit failed"
          continue
        fi
# serve：setcon adbd 可能瞬时 EACCES（AVC/KSHIELD 状态），用 20s 间隔重试，
# 失败子进程会立即退出（不 leak fd），总共约 4 分钟。
echo "serving" > "$STAT"
served=0
si=0
while [ $si -lt 12 ]; do
  si=$((si+1))
  echo serve > "$TMP/ghost_cmd"
  sleep 2
  if pidof glsrv >/dev/null 2>&1; then
    served=1
    break
  fi
  log "serve attempt $si failed, wait 20s"
  sleep 18
done
if [ $served -eq 1 ]; then
  echo "setlabel $TMP/gl-su u:object_r:shell_exec:s0" > "$TMP/ghost_cmd"
  sleep 1
  echo "ready" > "$STAT"
  log "ROOT_READY"
else
  echo "serve_failed" > "$STAT"
  log "serve failed after $si attempts"
  grep "\[SV\]" "$TMP/prog.log" 2>/dev/null | tail -n 6 >> "$LOG"
fi
        ;;
      stop)
        log "stop requested"
        echo "idle" > "$STAT"
        exit 0
        ;;
    esac
  fi
  sleep 1
done